//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2021 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>
#include <base/ovsocket/ovsocket.h>

#include <unordered_map>

#include "../http_datastructure.h"
#include "../http_error.h"
#include "../parser/http_response_parser.h"

namespace http
{
	namespace clnt
	{
		class HttpClient : public ov::EnableSharedFromThis<HttpClient>,
						   public ov::SocketAsyncInterface
		{
		public:
			// Use default TCP socket pool
			HttpClient();
			// Use specified socket pool
			HttpClient(const std::shared_ptr<ov::SocketPool> &socket_pool);

			~HttpClient() override;

			void SetBlockingMode(ov::BlockingMode mode);
			ov::BlockingMode GetBlockingMode() const;

			// timeout_msec == 0 means Infinite
			void SetConnectionTimeout(int timeout_msec);
			int GetConnectionTimeout() const;

			// timeout_msec == 0 means Infinite
			void SetRecvTimeout(int timeout_msec);
			int GetRecvTimeout() const;

			void SetTimeout(int timeout_msec);

			void SetMethod(http::Method method);
			http::Method GetMethod() const;

			// Request headers (Headers to sent to HTTP server)
			void SetRequestHeader(const ov::String &key, const ov::String &value);
			ov::String GetRequestHeader(const ov::String &key);
			const std::unordered_map<ov::String, ov::String, ov::CaseInsensitiveComparator> &GetRequestHeaders() const;
			std::unordered_map<ov::String, ov::String, ov::CaseInsensitiveComparator> &GetRequestHeaders();

			// HttpClient can send a request body even when the method is GET, but the server may not actually accept it
			void SetRequestBody(const std::shared_ptr<const ov::Data> &body);
			void SetRequestBody(const ov::String &body)
			{
				SetRequestBody(body.ToData(false));
			}

			void Request(const ov::String &url, ResponseHandler response_handler);

			// Response headers (Headers received from HTTP server)
			ov::String GetResponseHeader(const ov::String &key);
			const std::unordered_map<ov::String, ov::String, ov::CaseInsensitiveComparator> &GetResponseHeaders() const;

		protected:
			std::shared_ptr<const ov::Error> PrepareForRequest(const ov::String &url, ov::SocketAddress *address);
			void SendRequest();
			// Use this API when blocking mode
			void RecvResponse();
			std::shared_ptr<const ov::Error> ProcessData(const std::shared_ptr<const ov::Data> &data);

			void PostProcess();

			//--------------------------------------------------------------------
			// Implementation of SocketAsyncInterface
			//--------------------------------------------------------------------
			void OnConnected(const std::shared_ptr<const ov::SocketError> &error) override;
			void OnReadable() override;
			void OnClosed() override;

			void CleanupVariables();

		protected:
			std::shared_ptr<ov::SocketPool> _socket_pool;

			ov::BlockingMode _blocking_mode = ov::BlockingMode::Blocking;
			// Default: 10 seconds
			int _connection_timeout_msec = 10 * 1000;
			// Default: 60 seconds
			int _recv_timeout_msec = 60 * 1000;
			http::Method _method = http::Method::Get;

			HttpResponseParser _parser;

			std::mutex _request_mutex;
			std::atomic<bool> _requested = false;

			ov::String _url;
			std::shared_ptr<ov::Url> _parsed_url;
			ResponseHandler _response_handler = nullptr;

			std::shared_ptr<ov::Socket> _socket;

			std::unordered_map<ov::String, ov::String, ov::CaseInsensitiveComparator> _request_header;
			std::shared_ptr<ov::Data> _request_body;

			// response header
			bool _is_header_found = false;
			// A temporary string buffer to extract headers
			ov::String _response_string;
			std::map<ov::String, ov::String, ov::CaseInsensitiveComparator> _response_header;

			std::shared_ptr<ov::Data> _response_body;
		};
	}  // namespace clnt
}  // namespace http
