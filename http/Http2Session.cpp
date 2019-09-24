#ifdef WITH_NGHTTP2

#include "Http2Session.h"

static nghttp2_nv make_nv(const char* name, const char* value) {
    nghttp2_nv nv;
    nv.name = (uint8_t*)name;
    nv.value = (uint8_t*)value;
    nv.namelen = strlen(name);
    nv.valuelen = strlen(value);
    nv.flags = NGHTTP2_NV_FLAG_NONE;
    return nv;
}

static nghttp2_nv make_nv2(const char* name, const char* value,
        int namelen, int valuelen) {
    nghttp2_nv nv;
    nv.name = (uint8_t*)name;
    nv.value = (uint8_t*)value;
    nv.namelen = namelen; nv.valuelen = valuelen;
    nv.flags = NGHTTP2_NV_FLAG_NONE;
    return nv;
}

static void print_frame_hd(const nghttp2_frame_hd* hd) {
    printd("[frame] length=%d type=%x flags=%x stream_id=%d\n",
        (int)hd->length, (int)hd->type, (int)hd->flags, hd->stream_id);
}
static int on_header_callback(nghttp2_session *session,
        const nghttp2_frame *frame,
        const uint8_t *name, size_t namelen,
        const uint8_t *value, size_t valuelen,
        uint8_t flags, void *userdata);
static int on_data_chunk_recv_callback(nghttp2_session *session,
        uint8_t flags, int32_t stream_id, const uint8_t *data,
        size_t len, void *userdata);
static int on_frame_recv_callback(nghttp2_session *session,
        const nghttp2_frame *frame, void *userdata);
/*
static ssize_t data_source_read_callback(nghttp2_session *session,
        int32_t stream_id, uint8_t *buf, size_t length,
        uint32_t *data_flags, nghttp2_data_source *source, void *userdata);
*/


Http2Session::Http2Session(http_session_type type) {
    if (cbs == NULL) {
        nghttp2_session_callbacks_new(&cbs);
        nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_callback);
    }
    if (type == HTTP_CLIENT) {
        nghttp2_session_client_new(&session, cbs, NULL);
        state = HSS_SEND_MAGIC;
    }
    else if (type == HTTP_SERVER) {
        nghttp2_session_server_new(&session, cbs, NULL);
    }
    nghttp2_session_set_user_data(session, this);
    submited = NULL;
    parsed = NULL;
    stream_id = -1;
    stream_closed = 0;

    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
    };
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, ARRAY_SIZE(settings));
    state = HSS_SEND_SETTINGS;

    //nghttp2_submit_ping(session, NGHTTP2_FLAG_NONE, NULL);
    //state = HSS_SEND_PING;
}

Http2Session::~Http2Session() {
    if (session) {
        nghttp2_session_del(session);
        session = NULL;
    }
}

int Http2Session::GetSendData(char** data, size_t* len) {
    // HTTP2_MAGIC,HTTP2_SETTINGS,HTTP2_HEADERS
    *len = nghttp2_session_mem_send(session, (const uint8_t**)data);
    printd("nghttp2_session_mem_send %d\n", *len);
    if (*len != 0) return *len;

    if (submited == NULL) return 0;
    // HTTP2_DATA
    if (state == HSS_SEND_HEADERS) {
        void* content = submited->Content();
        int content_length = submited->ContentLength();
        // HTTP2 DATA framehd
        state = HSS_SEND_DATA_FRAME_HD;
        http2_frame_hd  framehd;
        framehd.length = content_length;
        framehd.type = HTTP2_DATA;
        framehd.flags = HTTP2_FLAG_END_STREAM;
        framehd.stream_id = stream_id;
        *data = (char*)frame_hdbuf;
        *len = HTTP2_FRAME_HDLEN;
        printd("HTTP2 DATA framehd-------------------\n");
        if (submited->ContentType() == APPLICATION_GRPC) {
            printd("grpc DATA framehd-----------------\n");
            grpc_message_hd msghd;
            msghd.flags = 0;
            msghd.length = content_length;

            if (type == HTTP_SERVER) {
                // grpc server send grpc-status in HTTP2 header frame
                framehd.flags = HTTP2_FLAG_NONE;

#ifdef TEST_PROTOBUF
                // @test protobuf
                // message StringMessage {
                //     string str = 1;
                // }
                int protobuf_taglen = 0;
                int tag = PROTOBUF_MAKE_TAG(1, WIRE_TYPE_LENGTH_DELIMITED);
                unsigned char* p = frame_hdbuf + HTTP2_FRAME_HDLEN + GRPC_MESSAGE_HDLEN;
                int bytes = varint_encode(tag, p);
                protobuf_taglen += bytes;
                p += bytes;
                bytes = varint_encode(content_length, p);
                protobuf_taglen += bytes;
                msghd.length += protobuf_taglen;
                framehd.length += protobuf_taglen;
                *len += protobuf_taglen;
#endif
            }

            grpc_message_hd_pack(&msghd, frame_hdbuf + HTTP2_FRAME_HDLEN);
            framehd.length += GRPC_MESSAGE_HDLEN;
            *len += GRPC_MESSAGE_HDLEN;
        }
        http2_frame_hd_pack(&framehd, frame_hdbuf);
    }
    else if (state == HSS_SEND_DATA_FRAME_HD) {
        // HTTP2 DATA
        printd("HTTP2 DATA-------------------\n");
        void* content = submited->Content();
        int content_length = submited->ContentLength();
        if (content_length == 0) {
            state = HSS_SEND_DONE;
        }
        else {
            state = HSS_SEND_DATA;
            *data = (char*)content;
            *len = content_length;
        }
    }
    else if (state == HSS_SEND_DATA) {
        state = HSS_SEND_DONE;
        if (submited->ContentType() == APPLICATION_GRPC) {
            if (type == HTTP_SERVER && stream_closed) {
                // grpc HEADERS grpc-status
                printd("grpc HEADERS grpc-status-----------------\n");
                int flags = NGHTTP2_FLAG_END_STREAM | NGHTTP2_FLAG_END_HEADERS;
                nghttp2_nv nv = make_nv("grpc-status", "0");
                nghttp2_submit_headers(session, flags, stream_id, NULL, &nv, 1, NULL);
                *len = nghttp2_session_mem_send(session, (const uint8_t**)data);
            }
        }
    }

    printd("GetSendData %d\n", *len);
    return *len;
}

int Http2Session::FeedRecvData(const char* data, size_t len) {
    printd("nghttp2_session_mem_recv %d\n", len);
    state = HSS_RECVING;
    size_t ret = nghttp2_session_mem_recv(session, (const uint8_t*)data, len);
    if (ret != len) {
        error = ret;
    }
    return (int)ret;
}

bool Http2Session::WantRecv() {
    if (stream_id == -1) return true;
    if (stream_closed) return false;
    if (state == HSS_RECV_DATA ||
        state == HSS_RECV_PING) {
        return false;
    }
    return true;
}

int Http2Session::SubmitRequest(HttpRequest* req) {
    submited = req;

    req->FillContentType();
    req->FillContentLength();
    if (req->ContentType() == APPLICATION_GRPC) {
        req->method = HTTP_POST;
        req->headers["te"] = "trailers";
        req->headers["user-agent"] = "grpc-c++/1.16.0 grpc-c/6.0.0 (linux; nghttp2; hw)";
        req->headers["accept-encoding"] = "identity";
        req->headers["grpc-accept-encoding"] = "identity";
    }

    std::vector<nghttp2_nv> nvs;
    char c_str[256] = {0};
    req->ParseUrl();
    nvs.push_back(make_nv(":method", http_method_str(req->method)));
    nvs.push_back(make_nv(":path", req->path.c_str()));
    nvs.push_back(make_nv(":scheme", req->https ? "https" : "http"));
    if (req->port == 0 ||
        req->port == DEFAULT_HTTP_PORT ||
        req->port == DEFAULT_HTTPS_PORT) {
        nvs.push_back(make_nv(":authority", req->host.c_str()));
    }
    else {
        snprintf(c_str, sizeof(c_str), "%s:%d", req->host.c_str(), req->port);
        nvs.push_back(make_nv(":authority", c_str));
    }
    const char* name;
    const char* value;
    for (auto& header : req->headers) {
        name = header.first.c_str();
        value = header.second.c_str();
        strlower((char*)name);
        if (strcmp(name, "connection") == 0) {
            // HTTP2 default keep-alive
            continue;
        }
        if (strcmp(name, "content-length") == 0) {
            // HTTP2 have frame_hd.length
            continue;
        }
        nvs.push_back(make_nv2(name, value, header.first.size(), header.second.size()));
    }
    int flags = NGHTTP2_FLAG_END_HEADERS;
    // we set EOS on DATA frame
    stream_id = nghttp2_submit_headers(session, flags, -1, NULL, &nvs[0], nvs.size(), NULL);
    // avoid DATA_SOURCE_COPY, we do not use nghttp2_submit_data
    // nghttp2_data_provider data_prd;
    // data_prd.read_callback = data_source_read_callback;
    //stream_id = nghttp2_submit_request(session, NULL, &nvs[0], nvs.size(), &data_prd, NULL);
    stream_closed = 0;
    state = HSS_SEND_HEADERS;
    return 0;
}

int Http2Session::SubmitResponse(HttpResponse* res) {
    submited = res;

    res->FillContentType();
    res->FillContentLength();
    if (parsed && parsed->ContentType() == APPLICATION_GRPC) {
        // correct content_type: application/grpc
        if (res->ContentType() != APPLICATION_GRPC) {
            res->content_type = APPLICATION_GRPC;
            res->headers["content-type"] = http_content_type_str(APPLICATION_GRPC);
        }
        //res->headers["accept-encoding"] = "identity";
        //hss->state = HSS_RECV_PING;
        //break;
        //res->headers["grpc-accept-encoding"] = "identity";
        //res->headers["grpc-status"] = "0";
#ifdef TEST_PROTOBUF
        res->status_code = HTTP_STATUS_OK;
#endif
    }

    std::vector<nghttp2_nv> nvs;
    char c_str[256] = {0};
    snprintf(c_str, sizeof(c_str), "%d", res->status_code);
    nvs.push_back(make_nv(":status", c_str));
    const char* name;
    const char* value;
    for (auto& header : res->headers) {
        name = header.first.c_str();
        value = header.second.c_str();
        strlower((char*)name);
        if (strcmp(name, "connection") == 0) {
            // HTTP2 default keep-alive
            continue;
        }
        if (strcmp(name, "content-length") == 0) {
            // HTTP2 have frame_hd.length
            continue;
        }
        nvs.push_back(make_nv2(name, value, header.first.size(), header.second.size()));
    }
    int flags = NGHTTP2_FLAG_END_HEADERS;
    // we set EOS on DATA frame
    if (stream_id == -1) {
        // upgrade
        nghttp2_session_upgrade(session, NULL, 0, NULL);
        stream_id = 1;
    }
    nghttp2_submit_headers(session, flags, stream_id, NULL, &nvs[0], nvs.size(), NULL);
    // avoid DATA_SOURCE_COPY, we do not use nghttp2_submit_data
    // data_prd.read_callback = data_source_read_callback;
    //nghttp2_submit_response(session, stream_id, &nvs[0], nvs.size(), &data_prd);
    stream_closed = 0;
    state = HSS_SEND_HEADERS;
    return 0;
}

int Http2Session::InitResponse(HttpResponse* res) {
    res->Reset();
    res->http_major = 2;
    res->http_minor = 0;
    parsed = res;
    return 0;
}

int Http2Session::InitRequest(HttpRequest* req) {
    req->Reset();
    req->http_major = 2;
    req->http_minor = 0;
    parsed = req;
    return 0;
}

int Http2Session::GetError() {
    return error;
}

const char* Http2Session::StrError(int error) {
    return nghttp2_http2_strerror(error);
}

nghttp2_session_callbacks* Http2Session::cbs = NULL;

int on_header_callback(nghttp2_session *session,
    const nghttp2_frame *frame,
    const uint8_t *_name, size_t namelen,
    const uint8_t *_value, size_t valuelen,
    uint8_t flags, void *userdata) {
    printd("on_header_callback\n");
    print_frame_hd(&frame->hd);
    const char* name = (const char*)_name;
    const char* value = (const char*)_value;
    printd("%s: %s\n", name, value);
    Http2Session* hss = (Http2Session*)userdata;
    if (*name == ':') {
        if (hss->parsed->type == HTTP_REQUEST) {
            // :method :path :scheme :authority
            HttpRequest* req = (HttpRequest*)hss->parsed;
            if (strcmp(name, ":method") == 0) {
                req->method = http_method_enum(value);
            }
            else if (strcmp(name, ":path") == 0) {
                req->url = value;
            }
            else if (strcmp(name, ":scheme") == 0) {
                req->headers["Scheme"] = value;
            }
            else if (strcmp(name, ":authority") == 0) {
                req->headers["Host"] = value;
            }
        }
        else if (hss->parsed->type == HTTP_RESPONSE) {
            HttpResponse* res = (HttpResponse*)hss->parsed;
            if (strcmp(name, ":status") == 0) {
                res->status_code = (http_status)atoi(value);
            }
        }
    }
    else {
        hss->parsed->headers[name] = value;
        if (strcmp(name, "content-type") == 0) {
            hss->parsed->content_type = http_content_type_enum(value);
        }
    }
    return 0;
}

int on_data_chunk_recv_callback(nghttp2_session *session,
    uint8_t flags, int32_t stream_id, const uint8_t *data,
    size_t len, void *userdata) {
    printd("on_data_chunk_recv_callback\n");
    printd("stream_id=%d length=%d\n", stream_id, (int)len);
    //printd("%.*s\n", (int)len, data);
    Http2Session* hss = (Http2Session*)userdata;

    if (hss->parsed->ContentType() == APPLICATION_GRPC) {
        // grpc_message_hd
        printd("grpc Length-Prefixed-Message-----------------\n");
        if (len >= GRPC_MESSAGE_HDLEN) {
            data += GRPC_MESSAGE_HDLEN;
            len -= GRPC_MESSAGE_HDLEN;
        }
    }
    hss->parsed->body.insert(hss->parsed->body.size(), (const char*)data, len);
    return 0;
}

int on_frame_recv_callback(nghttp2_session *session,
    const nghttp2_frame *frame, void *userdata) {
    printd("on_frame_recv_callback\n");
    print_frame_hd(&frame->hd);
    Http2Session* hss = (Http2Session*)userdata;
    switch (frame->hd.type) {
    case NGHTTP2_DATA:
    case NGHTTP2_HEADERS:
        hss->stream_id = frame->hd.stream_id;
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            printd("on_stream_closed stream_id=%d\n", hss->stream_id);
            hss->stream_closed = 1;
        }
        break;
    default:
        break;
    }

    switch (frame->hd.type) {
    case NGHTTP2_DATA:
        hss->state = HSS_RECV_DATA;
        break;
    case NGHTTP2_HEADERS:
        hss->state = HSS_RECV_HEADERS;
        break;
    case NGHTTP2_SETTINGS:
        hss->state = HSS_RECV_SETTINGS;
        break;
    case NGHTTP2_PING:
        hss->state = HSS_RECV_PING;
        break;
    default:
        break;
    }

    return 0;
}

#endif