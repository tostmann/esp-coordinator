
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
        if (ResolveStrategy::need_resolve()) {
            uint8_t outdata[Cmd::resp_buffer_size+sizeof(zb_ncp::cmd_t)];
            zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
            ResolveStrategy::resolve(*out_cmd);
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
	static zb_ncp::cmd_t m_cmd;
	// Returns true if the single slot was free and is now reserved for `cmd`.
	// Returns false if a prior async invocation of the same command is still
	// pending — caller should reject the new request rather than silently
	// overwriting the saved cmd (which loses the first one's tsn/seq and
	// guarantees the host times out on it).
	static bool start_resolve(const zb_ncp::cmd_t& cmd) {
		if (m_cmd.command_id == CmdId) {
			return false;
		}
		m_cmd = cmd;
		return true;
	}
	static bool need_resolve() {
		return m_cmd.command_id == CmdId;
	}
	static void resolve(zb_ncp::cmd_t& cmd) {
		cmd = m_cmd;
		m_cmd.command_id = command_id_t(0);
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

template <command_id_t CmdId,typename ArgType>
struct request_cmd_resolver {
	using Arg = ArgType;
	struct request_t {
		Arg arg;
		zb_ncp::cmd_t cmd;
		size_t old;
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
				// Clear state so a late ZBOSS callback for the abandoned tsn
				// no longer matches this slot in resolve() — without this, a
				// stale response could be routed to whichever new request the
				// slot is about to be reassigned to (potential mis-dispatch
				// of e.g. an IEEE_ADDR response to a NODE_DESC handler).
				res->state = request_t::S_NONE;
			}
		}
		if (res) {
			res->cmd = cmd;
			res->old = old_cntr()++;
		}
		return res;
	}

	static uint16_t get_req_idx(const request_t* req) {
		return req - storage();
	}
	static request_t& get_by_index(uint16_t idx) {
		return storage()[idx];
	}
	static request_t* resolve(uint8_t tsn) {
		for (auto& req:storage()) {
			if (req.state == request_t::S_EXEC && req.tsn == tsn) {
				return &req;
			}
		}
		return nullptr;
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
    
    static void handle_response(ResolveStrategy::request_t& req,const Resp* resp) {
    	uint8_t outdata[Cmd::resp_buffer_size+sizeof(zb_ncp::cmd_t)];
        zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
        *out_cmd = req.cmd;
        out_cmd->type = zb_ncp::RESPONSE;
        auto outlen = sizeof(zb_ncp::cmd_t);
        outlen += Cmd::format_response(reinterpret_cast<uint8_t*>(out_cmd+1),resp);
        zb_ncp::send_cmd_data( outdata, outlen ); 
    }
    static void req_cb(uint8_t buf) {
        auto zdp_cmd = static_cast<const zb_uint8_t*>(zb_buf_begin(buf));
        auto resp = reinterpret_cast<const Resp*>(zdp_cmd);
        auto req = ResolveStrategy::resolve(resp_parser<Resp>::get_tsn(resp));
        if (req) {
        	ESP_LOGD(TAG,"%s::req_cb %d",Cmd::name,int(resp_parser<Resp>::get_tsn(resp)));
        	auto status = resp_parser<Resp>::get_status(resp);
        	if (status == 0) {
        		 Cmd::handle_response(*req,resp);
        	} else {
        		report_failed(req->cmd,status);
        	}
        	req->state = ResolveStrategy::request_t::S_NONE;
        } else {
        	ESP_LOGW(TAG,"%s not found request for response %d",Cmd::name,int(resp_parser<Resp>::get_tsn(resp)));
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
            req.state = ResolveStrategy::request_t::S_NONE;
            report_failed(req.cmd,GENERIC_NO_RESOURCES);
        } else {
        	req.state = ResolveStrategy::request_t::S_EXEC;
        	req.tsn = r;
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

