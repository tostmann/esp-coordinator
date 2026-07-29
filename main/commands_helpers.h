
typedef uint16_t __attribute__ ((aligned(1))) unaligned_uint16_t;

struct generic_response_t{
	ncp_status_category_t category;
	ncp_generic_status_t status;
}__attribute__((packed));

template <typename Cmd>
struct cmd_base {
	static constexpr ncp_status_category_t status_category = STATUS_CATEGORY_GENERIC;

	static void report_status(uint8_t status,generic_response_t& resp) {
        ESP_LOGD(TAG,"%s report_status: %s (%d)",Cmd::name,utils::get_zdp_status_str(status),status);
        resp.category = Cmd::status_category;
        resp.status = static_cast<ncp_generic_status_t>(status);
    }
	static void report_failed(const zb_ncp::cmd_t& src_cmd, uint8_t status) {
		uint8_t outdata[sizeof(generic_response_t)+sizeof(zb_ncp::cmd_t)];
    	zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
    	*out_cmd = src_cmd;
    	out_cmd->type = zb_ncp::RESPONSE;
    	report_status(status,*reinterpret_cast<generic_response_t*>(out_cmd+1));
    	zb_ncp::send_cmd_data( outdata, sizeof(outdata) );
    }
	// Identical wire layout to report_failed (category + status, no body) — a
	// separate name for the call sites that report a SUCCESS this way, because a
	// command whose ZBOSS "response" carries no usable body has nothing else to
	// send. See request_cmd_process::response_body_is_synthetic.
	static void report_status_only(const zb_ncp::cmd_t& src_cmd, uint8_t status) {
		report_failed(src_cmd,status);
	}
};

template <command_id_t CmdId>
struct zb_ncp::immediate_cmd_process {
	using Cmd = cmd_handle<CmdId>;
    static void process(const zb_ncp::cmd_t& cmd, const void *buffer, size_t len) {
        uint8_t outdata[Cmd::resp_buffer_size+sizeof(zb_ncp::cmd_t)];
        zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
        auto outlen = sizeof(zb_ncp::cmd_t) + Cmd::process_immediate(buffer,len,&outdata[sizeof(zb_ncp::cmd_t)],sizeof(outdata)-sizeof(zb_ncp::cmd_t));
       	*out_cmd = cmd;
       	out_cmd->type = RESPONSE;
        zb_ncp::send_cmd_data( outdata, outlen ); 
    }
};

template <command_id_t CmdId,template<command_id_t> typename ResolveStrategyT>
struct zb_ncp::delayed_cmd_process : public ResolveStrategyT<CmdId>{
	using Cmd = cmd_handle<CmdId>;
	using ResolveStrategy = ResolveStrategyT<CmdId>;
    static void process(const zb_ncp::cmd_t& cmd, const void *buffer, size_t len) {
    	if (!ResolveStrategy::start_resolve(cmd)) {
    		// Slot already holds a prior async request — reject the duplicate
    		// immediately so the host's outer timeout doesn't fire on it.
    		// The in-flight original is left untouched and will complete
    		// normally via zboss_signal_handler -> response().
    		//
    		// Build the BUSY reply inline rather than going through
    		// cmd_base<Cmd>::report_failed: cmd_base is CRTP and wants
    		// Cmd::status_category, which the NWK_FORMATION /
    		// NWK_START_WITHOUT_FORMATION handlers don't define (they go
    		// through finish_delayed for their own STATUS_CATEGORY_NWK
    		// payload). Synthetic BUSY is generic anyway.
    		ESP_LOGW(TAG, "%s: busy, prior request pending", Cmd::name);
    		uint8_t outdata[sizeof(zb_ncp::cmd_t) + sizeof(generic_response_t)];
    		auto out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
    		*out_cmd = cmd;
    		out_cmd->type = zb_ncp::RESPONSE;
    		auto resp = reinterpret_cast<generic_response_t*>(out_cmd + 1);
    		resp->category = STATUS_CATEGORY_GENERIC;
    		resp->status   = GENERIC_BUSY;
    		zb_ncp::send_cmd_data(outdata, sizeof(outdata));
    		return;
    	}
        int res = Cmd::start_delayed(buffer,len);
        if (res != 0) {
            ESP_LOGE(TAG,"%s:process_delayed:process failed start_delayed",Cmd::name);
            response(res);
        }
    }
    static bool response(int status) {
        zb_ncp::cmd_t saved_cmd;
        if (ResolveStrategy::resolve_take(saved_cmd)) {
            uint8_t outdata[Cmd::resp_buffer_size+sizeof(zb_ncp::cmd_t)];
            zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
            *out_cmd = saved_cmd;
            out_cmd->type = RESPONSE;
            auto outlen = sizeof(zb_ncp::cmd_t) + Cmd::finish_delayed(status,&outdata[sizeof(zb_ncp::cmd_t)],sizeof(outdata)-sizeof(zb_ncp::cmd_t));
            zb_ncp::send_cmd_data( outdata, outlen ); 
            return true;
        } 
        return false;
    }
};

template <command_id_t CmdId>
struct single_cmd_delayed {
	// HELP-4: command_id 0 is the "slot free" sentinel — resolve_take() clears
	// m_cmd.command_id to command_id_t(0), and start_resolve()/need_resolve()
	// test `m_cmd.command_id == CmdId`. This couples to commands_list.h's
	// numbering: any command that uses single_cmd_delayed must have a non-zero
	// id, otherwise its slot could never be marked free (resolve would leave it
	// looking still-pending). Enforced for each user below; documented here so a
	// future COMMAND(..., 0) entry on a delayed command fails loudly at compile
	// time rather than wedging that command at runtime.
	static_assert(CmdId != command_id_t(0),
	              "single_cmd_delayed: command_id 0 is reserved as the free sentinel");
	static zb_ncp::cmd_t m_cmd;

	// MECH-2: m_cmd is armed by the app task (start_resolve) and read/cleared by
	// the ZBOSS task (need_resolve / resolve_take) — the same cross-task data-race
	// class as DISP-2's request_cmd_resolver::storage. Guard every access with a
	// per-instantiation leaf mutex. All critical sections are tiny cmd_t copies;
	// no blocking work (send_cmd_data) runs under the lock, so it never nests with
	// request_resolver_mutex / m_tx_sem / m_input_sem → no lock-ordering cycle.
	static SemaphoreHandle_t mutex() {
		static SemaphoreHandle_t m = xSemaphoreCreateMutex();
		configASSERT(m != NULL);  // MECH-3: fail fast on init-time OOM
		return m;
	}
	// Returns true if the single slot was free and is now reserved for `cmd`.
	// Returns false if a prior async invocation of the same command is still
	// pending — caller should reject the new request rather than silently
	// overwriting the saved cmd (which loses the first one's tsn/seq and
	// guarantees the host times out on it).
	static bool start_resolve(const zb_ncp::cmd_t& cmd) {
		utils::sem_lock l(mutex());
		if (m_cmd.command_id == CmdId) {
			return false;
		}
		m_cmd = cmd;
		return true;
	}
	// Non-destructive peek: is a request for CmdId currently armed? Used by
	// continue_zboss() to choose formation-vs-steering without consuming the slot.
	static bool need_resolve() {
		utils::sem_lock l(mutex());
		return m_cmd.command_id == CmdId;
	}
	// MECH-2: atomic test-copy-clear, replacing the old need_resolve()+resolve()
	// two-acquisition sequence in delayed_cmd_process::response() (which left a
	// window where the slot could be re-armed/freed between the two calls). On a
	// hit, copies the armed cmd into `out` and frees the slot under the lock; the
	// blocking response send then runs on the caller's stack-local copy outside
	// the lock. Mirrors request_cmd_resolver::resolve_take.
	static bool resolve_take(zb_ncp::cmd_t& out) {
		utils::sem_lock l(mutex());
		if (m_cmd.command_id != CmdId) {
			return false;
		}
		out = m_cmd;
		m_cmd.command_id = command_id_t(0);
		return true;
	}
};
#define SINGLE_CMD_DELAYED_DECL(CmdId) \
template <> \
zb_ncp::cmd_t single_cmd_delayed<CmdId>::m_cmd = {};

template <command_id_t CmdId,typename Res>
struct zb_ncp::general_status_res {
	struct FullRes {
		generic_response_t status;
		Res res;
	} __attribute__((packed)) __attribute__ ((aligned(1)));
	static constexpr size_t resp_buffer_size = sizeof(FullRes);
	static size_t process_result(uint8_t* outdata,size_t outdata_size) {
		auto full_res = reinterpret_cast<FullRes*>(outdata);
		full_res->status.category = STATUS_CATEGORY_GENERIC;
		full_res->status.status = GENERIC_OK;
		zb_ncp::cmd_handle<CmdId>::process_status_res(full_res->status.status,&full_res->res);
        return sizeof(FullRes);
    }
    static size_t process_immediate(const void *inbuffer, size_t inlen,uint8_t* outdata,size_t outdata_size) {
		return zb_ncp::cmd_handle<CmdId>::process_result(outdata,outdata_size);
    }
};



template <command_id_t CmdId,typename Arg>
struct zb_ncp::general_status_arg {
	struct FullRes {
		generic_response_t status;
	} __attribute__((packed)) __attribute__ ((aligned(1)));
	static constexpr size_t resp_buffer_size = sizeof(FullRes);
	
    static size_t process_immediate(const void *inbuffer, size_t inlen,uint8_t* outdata,size_t outdata_size) {
    	auto full_res = reinterpret_cast<FullRes*>(outdata);
		full_res->status.category = STATUS_CATEGORY_GENERIC;
		full_res->status.status = GENERIC_OK;
		if (inlen < sizeof(Arg)) {
			full_res->status.status = GENERIC_INVALID_PARAMETER;
			return sizeof(FullRes);
		}
		auto arg = static_cast<const Arg*>(inbuffer);
		zb_ncp::cmd_handle<CmdId>::process_status_arg(full_res->status.status,*arg);
		return sizeof(FullRes);
    }
};

template <command_id_t CmdId,typename Arg,typename Res>
struct zb_ncp::general_status_arg_res {
	struct FullRes {
		generic_response_t status;
		Res res;
	} __attribute__((packed)) __attribute__ ((aligned(1)));
	static constexpr size_t resp_buffer_size = sizeof(FullRes);
	static size_t process_immediate(const void *inbuffer, size_t inlen,uint8_t* outdata,size_t outdata_size) {
    	auto full_res = reinterpret_cast<FullRes*>(outdata);
		full_res->status.category = STATUS_CATEGORY_GENERIC;
		full_res->status.status = GENERIC_OK;
		if (inlen < sizeof(Arg)) {
			full_res->status.status = GENERIC_INVALID_PARAMETER;
			return sizeof(FullRes);
		}
		auto arg = static_cast<const Arg*>(inbuffer);
		zb_ncp::cmd_handle<CmdId>::process_status_arg_res(full_res->status.status,*arg,&full_res->res);
		return sizeof(FullRes);
    }
};

// DISP-2: serializes all request_cmd_resolver::storage() access. start_resolve()
// runs on the app task (on_rx_data → process), while resolve_take() and the
// do_request() state transitions run on the ZBOSS task (req_cb / aps tx callback
// / data_indication). On the single-core C6 the ZBOSS task (prio 5) preempts the
// app task (prio 1) but never vice-versa, and a ZBOSS callback can yield mid-send
// (transport backpressure) letting the app task run — so the two-task interleave
// on the slot array is real. One shared mutex across all resolver instantiations
// is enough: the critical sections are short table scans, cross-resolver
// contention is negligible, and FreeRTOS mutex priority inheritance bounds the
// app-holds-while-ZBOSS-waits case (start_resolve never blocks while holding it).
// Lazily created; the first call is the app task's first request, strictly
// happens-before any ZBOSS callback for that request, so the init is unraced.
static SemaphoreHandle_t request_resolver_mutex() {
	static SemaphoreHandle_t m = xSemaphoreCreateMutex();
	configASSERT(m != NULL);  // MECH-3: fail fast on init-time OOM (matches single_cmd_delayed::mutex)
	return m;
}

template <command_id_t CmdId,typename ArgType>
struct request_cmd_resolver {
	using Arg = ArgType;
	struct request_t {
		Arg arg;
		zb_ncp::cmd_t cmd;
		size_t old;
		uint32_t t_exec;   // FreeRTOS tick when the slot entered S_EXEC (watchdog)
		uint8_t tsn;
		enum state_t : uint8_t {
			S_NONE,
			S_ALLOCATION,
			S_EXEC,
		} state;
	};
	using requests_arr_t = request_t[zb_ncp::MAX_PARALLEL_REQUESTS];
	static size_t& old_cntr() {
		static size_t s = 0;
		return s;
	}
	static requests_arr_t& storage() {
		static requests_arr_t s_requests;
		return s_requests;
	} 
	
	static request_t* start_resolve(const zb_ncp::cmd_t& cmd) {
		// DISP-2: runs on the app task. resolve_take()/do_request() mutate the
		// same storage() from the ZBOSS task — hold the shared resolver mutex
		// across scan+claim so the two tasks never interleave a read-decide-write
		// on the slot array. Released before the (possibly blocking)
		// zb_buf_get_out_delayed_ext() in the caller, so the app task never
		// blocks while holding it (bounds priority inheritance).
		utils::sem_lock l(request_resolver_mutex());
		request_t* res = nullptr;
		for (auto& req:storage()) {
			if (req.state == request_t::S_NONE) {
				res = &req;
				break;
			}
		}
		if (!res) {
			// All MAX_PARALLEL_REQUESTS slots are busy. Find the OLDEST S_EXEC
			// slot (smallest `old` counter) and reuse it. The previous code
			// scanned with `req.old < old` where `old = old_cntr()` is the
			// current global counter — every S_EXEC slot trivially matched and
			// `res` ended up pointing at the LAST one iterated, not the oldest.
			size_t oldest = SIZE_MAX;
			for (auto& req:storage()) {
				if (req.state == request_t::S_EXEC && req.old < oldest) {
					oldest = req.old;
					res = &req;
				}
			}
			if (res) {
				ESP_LOGW(TAG,"Override request tsn=%d old=%zu",
				         int(res->tsn), oldest);
			}
		}
		if (res) {
			res->cmd = cmd;
			res->old = old_cntr()++;
			// Reserve under the lock. S_ALLOCATION is matched by neither the
			// free-scan (S_NONE) above nor resolve_take() (S_EXEC), so once
			// reserved no other task can re-pick or free this slot until the
			// caller's do_request transitions it to S_EXEC. Setting it here (vs
			// in the caller, outside the lock) is also what makes the
			// abandoned-tsn override safe: a late ZBOSS callback for the old tsn
			// no longer matches this slot (state != S_EXEC), so it can't be
			// mis-routed to the request the slot is about to carry.
			res->state = request_t::S_ALLOCATION;
		}
		return res;
	}

	static uint16_t get_req_idx(const request_t* req) {
		return req - storage();
	}
	static request_t& get_by_index(uint16_t idx) {
		return storage()[idx];
	}
	// DISP-2: atomically find the S_EXEC slot matching `tsn`, copy it out, and
	// free it (S_NONE) — all under the resolver mutex. Callers run on the ZBOSS
	// task and then do BLOCKING work (handle_response → send_cmd_data →
	// transport, which can vTaskDelay on backpressure). The old resolve() returned
	// a pointer and the caller freed the slot only AFTER that blocking send,
	// leaving a window in which the app task's start_resolve override could
	// re-pick the still-S_EXEC slot and reassign it — then the late S_NONE write
	// clobbered a fresh request. Copying out + freeing up front closes that
	// window: the blocking send then runs on a local copy nobody else can touch.
	static bool resolve_take(uint8_t tsn, request_t& out) {
		utils::sem_lock l(request_resolver_mutex());
		for (auto& req:storage()) {
			if (req.state == request_t::S_EXEC && req.tsn == tsn) {
				out = req;
				req.state = request_t::S_NONE;
				return true;
			}
		}
		return false;
	}

	// Watchdog reclaim: free every S_EXEC slot whose TX-confirm never came back
	// within `timeout_ticks` (an unreachable/dead device whose confirm never
	// fires would otherwise leak the slot forever — see the apsde-slot-leak
	// note). Copies the abandoned cmds into `out` and frees the slots UNDER the
	// lock; the caller sends the host failures AFTER releasing it (report_failed
	// can block on transport), exactly like resolve_take. Returns the count.
	static size_t reclaim_stale(uint32_t now_ticks, uint32_t timeout_ticks,
	                            zb_ncp::cmd_t* out, size_t out_cap) {
		utils::sem_lock l(request_resolver_mutex());
		size_t n = 0;
		for (auto& req:storage()) {
			if (req.state == request_t::S_EXEC &&
			    (uint32_t)(now_ticks - req.t_exec) >= timeout_ticks) {
				if (n < out_cap) out[n++] = req.cmd;
				req.state = request_t::S_NONE;
			}
		}
		return n;
	}
};

template <typename Resp>
struct resp_parser {
    static inline uint8_t get_status(const Resp* resp) {
        return resp->status;
    }
    static inline uint8_t get_tsn(const Resp* resp) {
    	return resp->tsn;
    }
};
template <>
struct resp_parser<zb_zdo_simple_desc_resp_t> {
    static inline uint8_t get_status(const zb_zdo_simple_desc_resp_t* resp) {
        return resp->hdr.status;
    }
    static inline uint8_t get_tsn(const zb_zdo_simple_desc_resp_t* resp) {
    	return resp->hdr.tsn;
    }
};
template <>
struct resp_parser<zb_zdo_node_desc_resp_t> {
    static inline uint8_t get_status(const zb_zdo_node_desc_resp_t* resp) {
        return resp->hdr.status;
    }
    static inline uint8_t get_tsn(const zb_zdo_node_desc_resp_t* resp) {
    	return resp->hdr.tsn;
    }
};

template <>
struct resp_parser<zb_zdo_power_desc_resp_t> {
    static inline uint8_t get_status(const zb_zdo_power_desc_resp_t* resp) {
        return resp->hdr.status;
    }
    static inline uint8_t get_tsn(const zb_zdo_power_desc_resp_t* resp) {
    	return resp->hdr.tsn;
    }
};


template <command_id_t CmdId,typename Arg, typename Req, typename Resp>
struct zb_ncp::request_cmd_process : public request_cmd_resolver<CmdId,Arg> {
    static constexpr bool request_is_data = true;
    using Cmd = zb_ncp::cmd_handle<CmdId>;
    using ArgType = Arg;
    //using Base = delayed_cmd_process< CmdId , request_cmd_resolver >;
    using ResolveStrategy = request_cmd_resolver<CmdId,Arg>;
   
    static constexpr size_t resp_buffer_size = /*sizeof(generic_response_t) +*/ sizeof(Resp) + Cmd::additional_buffer_size;
    static constexpr size_t additional_buffer_size = 0;

    static constexpr ncp_status_category_t status_category = STATUS_CATEGORY_ZDO;

    static uint16_t do_finish(uint8_t* outdata,uint16_t outlen,int status) {
        return 0;
    }
    
    static void report_status(uint8_t status,generic_response_t& resp) {
    	cmd_base<Cmd>::report_status(status,resp);
    }

    static void report_failed(const zb_ncp::cmd_t& src_cmd, uint8_t status) {
    	cmd_base<Cmd>::report_failed(src_cmd,status);
    }

    static void report_status_only(const zb_ncp::cmd_t& src_cmd, uint8_t status) {
    	cmd_base<Cmd>::report_status_only(src_cmd,status);
    }

    static void handle_response(ResolveStrategy::request_t& req,const Resp* resp) {
    	uint8_t outdata[Cmd::resp_buffer_size+sizeof(zb_ncp::cmd_t)];
        zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
        *out_cmd = req.cmd;
        out_cmd->type = zb_ncp::RESPONSE;
        auto outlen = sizeof(zb_ncp::cmd_t);
        outlen += Cmd::format_response(reinterpret_cast<uint8_t*>(out_cmd+1),resp);
        zb_ncp::send_cmd_data( outdata, outlen ); 
    }
    // Hook for "responses" ZBOSS synthesizes instead of receiving off-air.
    // Default: the response is real, so its ZDP status byte and body are
    // authoritative. A Cmd that returns true declares the body unusable; req_cb
    // then answers the host with `status` alone (which the Cmd may rewrite).
    // Only user today: cmd_handle<ZDO_PERMIT_JOINING_REQ> — see the comment
    // there for the mechanism and the bench proof.
    static bool response_body_is_synthetic(const typename ResolveStrategy::request_t&,
                                           uint8_t&) {
        return false;
    }
    static void req_cb(uint8_t buf) {
        auto zdp_cmd = static_cast<const zb_uint8_t*>(zb_buf_begin(buf));
        auto resp = reinterpret_cast<const Resp*>(zdp_cmd);
        auto tsn = resp_parser<Resp>::get_tsn(resp);
        // DISP-2: claim+free the slot atomically, then work on the local copy so
        // the blocking handle_response/report_failed send can't race a concurrent
        // app-task start_resolve override on the slot.
        typename ResolveStrategy::request_t req;
        if (ResolveStrategy::resolve_take(tsn, req)) {
        	ESP_LOGD(TAG,"%s::req_cb %d",Cmd::name,int(tsn));
        	auto status = resp_parser<Resp>::get_status(resp);
        	if (Cmd::response_body_is_synthetic(req,status)) {
        		// No usable body — report the status on its own. For the 2-byte
        		// ZDO responses this is byte-identical to what a successful
        		// format_response() would emit, so hosts see a normal reply.
        		report_status_only(req.cmd,status);
        	} else if (status == 0) {
        		 Cmd::handle_response(req,resp);
        	} else {
        		report_failed(req.cmd,status);
        	}
        } else {
        	ESP_LOGW(TAG,"%s not found request for response %d",Cmd::name,int(tsn));
        }
        zb_buf_free(buf);
    }
    static void format_request(Req& req,const Arg& arg) {
    	req = arg;
    }
    static void on_request_started( ) {}
    static size_t get_request_alloc_size(const Arg& arg) {
    	return sizeof(Req);
    }
    static void do_request(uint8_t buf,uint16_t req_arg) {
    	Req* request_data;
    	auto& req = ResolveStrategy::get_by_index(req_arg);
    	// Slot is S_ALLOCATION here (reserved by start_resolve under the lock), so
    	// reading req.arg without the lock is safe: neither the app-task override
    	// (scans S_EXEC) nor resolve_take (scans S_EXEC) can touch an S_ALLOCATION
    	// slot, even if start_request below yields.
        if (Cmd::request_is_data) {
        	ESP_LOGD(TAG,"%s::do_request zb_buf_initial_alloc req_idx: %d",Cmd::name,req_arg);
            request_data = static_cast<Req*>(zb_buf_initial_alloc(buf, Cmd::get_request_alloc_size(req.arg)));
        } else {
        	ESP_LOGD(TAG,"%s::do_request zb_buf_alloc_tail req_idx: %d",Cmd::name,req_arg);
            request_data = static_cast<Req*>(zb_buf_alloc_tail(buf, Cmd::get_request_alloc_size(req.arg)));
        }
        Cmd::format_request(*request_data,req.arg);
        auto r = Cmd::start_request(buf);
        if (r == 0xFF) {
            ESP_LOGE(TAG,"%s::do_request failed",Cmd::name);
            zb_buf_free(buf);
            zb_ncp::cmd_t failed_cmd;
            {   // DISP-2: free the slot under the lock; report outside it
                // (report_failed → send can block).
                utils::sem_lock l(request_resolver_mutex());
                failed_cmd = req.cmd;
                req.state = ResolveStrategy::request_t::S_NONE;
            }
            report_failed(failed_cmd,GENERIC_NO_RESOURCES);
        } else {
            {   // DISP-2: publish tsn before S_EXEC, atomically w.r.t. a
                // concurrent app-task start_resolve override scan.
                utils::sem_lock l(request_resolver_mutex());
                req.tsn = r;
                req.t_exec = xTaskGetTickCount();
                req.state = ResolveStrategy::request_t::S_EXEC;
            }
            ESP_LOGD(TAG,"%s::do_request tsn: %d",Cmd::name,int(r));
            Cmd::on_request_started();
        }
    }
    static uint16_t format_response(uint8_t* outdata,const Resp* resp) {
        memcpy(outdata,resp,sizeof(Resp));
        outdata[0] = STATUS_CATEGORY_ZDO;
        return sizeof(Resp);
    }
    static bool check_arg_size(const void *buffer, size_t len) {
    	return len >= sizeof(Arg);
    }
    static esp_err_t process(const zb_ncp::cmd_t& cmd, const void *buffer, size_t len) {
    	if (!Cmd::check_arg_size(buffer,len)) {
    		report_failed(cmd,GENERIC_INVALID_PARAMETER);
    		return ESP_OK;
    	}
    	auto req = ResolveStrategy::start_resolve(cmd);
    	if (!req) {
    		report_failed(cmd,GENERIC_NO_RESOURCES);
    		return ESP_OK;
    	}
    	req->arg = *reinterpret_cast<const Arg*>(buffer);
    	req->state = ResolveStrategy::request_t::S_ALLOCATION;
        auto ret = zb_buf_get_out_delayed_ext(&do_request,ResolveStrategy::get_req_idx(req),Cmd::get_request_alloc_size(req->arg));
        if (ret != 0) {
        	req->state = ResolveStrategy::request_t::S_NONE;
        	report_failed(cmd,GENERIC_NO_RESOURCES);
        	return ESP_OK;
        } else {
        	ESP_LOGD(TAG,"%s::do_start req_idx: %d",Cmd::name,ResolveStrategy::get_req_idx(req));
        }
        return ESP_OK;
    }
};

