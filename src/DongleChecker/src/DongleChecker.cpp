// DongleChecker.cpp : Standalone 32-bit HTTP server for the /license endpoint.

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <json/json.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "HVHASPManager.h"
#include "hasp_api_cpp.h"
#include "tinyxml.h"

// Define BUILD_CONSOLE to run directly as a console application.

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

using json = nlohmann::json;

SERVICE_STATUS_HANDLE g_hSrv;
DWORD g_NowState;
BOOL g_bPause;
HANDLE ghSvcStopEvent = NULL;
#define SERVICE_NAME TEXT("DongleCheckerSvc")
void MyServiceMain(DWORD argc, LPTSTR* argv);
void MyServiceHandler(DWORD opCode);

namespace
{
	std::vector<std::string> split(const std::string& text, char delimiter)
	{
		std::istringstream stream(text);
		std::string token;
		std::vector<std::string> result;

		while (std::getline(stream, token, delimiter))
		{
			result.push_back(token);
		}

		return result;
	}

	std::string getServiceName(const std::string& target)
	{
		std::string path = target;
		const std::string::size_type queryPos = path.find('?');
		if (queryPos != std::string::npos)
		{
			path = path.substr(0, queryPos);
		}

		if (!path.empty() && path[0] == '/')
		{
			path.erase(0, 1);
		}

		const std::string::size_type separatorPos = path.find('/');
		if (separatorPos != std::string::npos)
		{
			path = path.substr(0, separatorPos);
		}

		return path;
	}

	std::string jsonEscape(const std::string& value)
	{
		std::ostringstream out;
		for (std::string::const_iterator it = value.begin(); it != value.end(); ++it)
		{
			switch (*it)
			{
			case '\\':
				out << "\\\\";
				break;
			case '"':
				out << "\\\"";
				break;
			case '\n':
				out << "\\n";
				break;
			case '\r':
				out << "\\r";
				break;
			case '\t':
				out << "\\t";
				break;
			default:
				out << *it;
				break;
			}
		}

		return out.str();
	}



	void fail(boost::system::error_code ec, const char* what)
	{
		std::cerr << what << ": " << ec.message() << "\n";
	}

	std::string serviceLicense(std::string jsonData)
	{
		json wcjson;

		const int kDefaultKey = -1;
		const char* kDefaultSN = "SN0000000";
		const char* kDefaultDate = "000000";

		CHVHASPManager hasp;
		bool isConnDongle = hasp.CheckConnectDongle();

		try {
			auto keyValue = hasp.HASPLogin();
			if (keyValue < 0)
			{
				wcjson["IsConn"] = isConnDongle;
				wcjson["Key"] = -1;
				wcjson["SerialNumber"] = "000000";
				wcjson["Date"] = "000000";
				jsonData = wcjson.dump(2, ' ');
				return jsonData;
			}

			std::string strSN;
			std::string strDate;

			bool isGetProductinfo = true;
			if (!hasp.GetProductInfo(strSN, strDate)) {
				wcjson["IsConn"] = isConnDongle;
				wcjson["Key"] = -1;
				wcjson["SerialNumber"] = "000000";
				wcjson["Date"] = "000000";
				jsonData = wcjson.dump(2, ' ');
				return jsonData;
			}

			wcjson["IsConn"] = isConnDongle;
			wcjson["Key"] = keyValue;

			std::string uniqueNumber = "";
			if (keyValue == 8) // HIIS
			{
				uniqueNumber = "HPS0000";
			}
			else
			{
				uniqueNumber = "Unknown_";
			}

			wcjson["SerialNumber"] = uniqueNumber + strSN;
			wcjson["Date"] = strDate;

			jsonData = wcjson.dump(2, ' ');
		}
		catch (const std::exception&) {
			wcjson["IsConn"] = isConnDongle;
			wcjson["Key"] = -1;
			wcjson["SerialNumber"] = "000000";
			wcjson["Date"] = "000000";
			jsonData = wcjson.dump(2, ' ');
			return jsonData;
		}
		return jsonData;
	}

	template<class Body, class Allocator, class Send>
	void handleRequest(
		http::request<Body, http::basic_fields<Allocator> >&& req,
		Send&& send)
	{
		const auto badRequest = [&req](boost::beast::string_view why) {
			http::response<http::string_body> res(http::status::bad_request, req.version());
			res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
			res.set(http::field::content_type, "text/plain");
			res.keep_alive(req.keep_alive());
			res.body() = why.to_string();
			res.prepare_payload();
			return res;
		};

		const auto notFound = [&req](boost::beast::string_view target) {
			http::response<http::string_body> res(http::status::not_found, req.version());
			res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
			res.set(http::field::content_type, "text/plain");
			res.keep_alive(req.keep_alive());
			res.body() = "The resource '" + target.to_string() + "' was not found.";
			res.prepare_payload();
			return res;
		};

		if (req.method() != http::verb::get &&
			req.method() != http::verb::post &&
			req.method() != http::verb::head)
		{
			return send(badRequest("Unknown HTTP-method"));
		}

		if (req.target() == "/health")
		{
			http::response<http::string_body> res(http::status::ok, req.version());
			res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(req.keep_alive());
			res.body() = "{\"status\":\"ok\"}";
			res.prepare_payload();
			return send(std::move(res));
		}

		if (req.target().empty() ||
			req.target()[0] != '/' ||
			req.target().find("..") != boost::beast::string_view::npos)
		{
			return send(badRequest("Illegal request-target"));
		}

		const std::string serviceName = getServiceName(req.target().to_string());
		if (serviceName == "license")
		{
			std::cout << "service_name=license" << std::endl;
			std::string response_data("");
			http::response<http::string_body> res(http::status::ok, req.version());
			res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
			res.set(http::field::content_type, "text/plain");

			response_data = serviceLicense(req.body());

			res.result(http::status::ok);
			//res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
			//res.set(http::field::content_type, "application/json");
			res.body() = response_data;
			res.prepare_payload();
			res.keep_alive(req.keep_alive());
			return send(std::move(res));
		}
		else
		{
			return send(notFound(req.target()));
		}


		//res.set(http::field::content_type, "application/json");
		//res.keep_alive(req.keep_alive());
		//res.body() = serviceLicense();
		//res.prepare_payload();
		//return send(std::move(res));
	}

	class HttpSession : public std::enable_shared_from_this<HttpSession>
	{
	public:
		explicit HttpSession(tcp::socket socket)
			: socket_(std::move(socket))
			, strand_(socket_.get_executor())
			, timer_(socket_.get_executor().context(), (std::chrono::steady_clock::time_point::max)())
		{
		}

		void run()
		{
			onTimer({});
			doRead();
		}

	private:
		class SendLambda
		{
		public:
			explicit SendLambda(HttpSession& self)
				: self_(self)
			{
			}

			template<bool isRequest, class Body, class Fields>
			void operator()(http::message<isRequest, Body, Fields>&& msg) const
			{
				typedef http::message<isRequest, Body, Fields> ResponseType;
				std::shared_ptr<ResponseType> sp = std::make_shared<ResponseType>(std::move(msg));
				self_.res_ = sp;

				http::async_write(
					self_.socket_,
					*sp,
					boost::asio::bind_executor(
						self_.strand_,
						std::bind(
							&HttpSession::onWrite,
							self_.shared_from_this(),
							std::placeholders::_1,
							std::placeholders::_2,
							sp->need_eof())));
			}

		private:
			HttpSession& self_;
		};

		void doRead()
		{
			req_ = {};
			timer_.expires_after(std::chrono::seconds(30));
			http::async_read(
				socket_,
				buffer_,
				req_,
				boost::asio::bind_executor(
					strand_,
					std::bind(
						&HttpSession::onRead,
						shared_from_this(),
						std::placeholders::_1)));
		}

		void onRead(boost::system::error_code ec)
		{
			if (ec == http::error::end_of_stream)
			{
				return doClose();
			}

			if (ec)
			{
				return fail(ec, "read");
			}

			handleRequest(std::move(req_), SendLambda(*this));
		}

		void onWrite(boost::system::error_code ec, std::size_t, bool close)
		{
			res_.reset();

			if (ec)
			{
				return fail(ec, "write");
			}

			if (close)
			{
				return doClose();
			}

			doRead();
		}

		void doClose()
		{
			boost::system::error_code ec;
			socket_.shutdown(tcp::socket::shutdown_send, ec);
		}

		void onTimer(boost::system::error_code ec)
		{
			if (ec && ec != boost::asio::error::operation_aborted)
			{
				return fail(ec, "timer");
			}

			if (timer_.expiry() <= std::chrono::steady_clock::now())
			{
				socket_.close(ec);
				timer_.expires_at((std::chrono::steady_clock::time_point::max)());
			}

			timer_.async_wait(
				boost::asio::bind_executor(
					strand_,
					std::bind(
						&HttpSession::onTimer,
						shared_from_this(),
						std::placeholders::_1)));
		}

		tcp::socket socket_;
		boost::asio::strand<boost::asio::io_context::executor_type> strand_;
		boost::asio::steady_timer timer_;
		boost::beast::flat_buffer buffer_;
		http::request<http::string_body> req_;
		std::shared_ptr<void> res_;
	};

	class Listener : public std::enable_shared_from_this<Listener>
	{
	public:
		Listener(boost::asio::io_context& ioc, tcp::endpoint endpoint)
			: acceptor_(ioc)
			, socket_(ioc)
		{
			boost::system::error_code ec;
			acceptor_.open(endpoint.protocol(), ec);
			if (ec)
			{
				fail(ec, "open");
				return;
			}

			acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
			if (ec)
			{
				fail(ec, "set_option");
				return;
			}

			acceptor_.bind(endpoint, ec);
			if (ec)
			{
				fail(ec, "bind");
				return;
			}

			acceptor_.listen(boost::asio::socket_base::max_connections, ec);
			if (ec)
			{
				fail(ec, "listen");
				return;
			}
		}

		void run()
		{
			if (!acceptor_.is_open())
			{
				return;
			}

			doAccept();
		}

	private:
		void doAccept()
		{
			acceptor_.async_accept(
				socket_,
				std::bind(
					&Listener::onAccept,
					shared_from_this(),
					std::placeholders::_1));
		}

		void onAccept(boost::system::error_code ec)
		{
			if (ec)
			{
				fail(ec, "accept");
			}
			else
			{
				std::make_shared<HttpSession>(std::move(socket_))->run();
			}

			doAccept();
		}

		tcp::acceptor acceptor_;
		tcp::socket socket_;
	};

	unsigned short getPortFromEnvironment(unsigned short defaultPort)
	{
		size_t size = 0;
		char* buffer = NULL;
		if (_dupenv_s(&buffer, &size, "HOCT_LICENSE_PORT") == 0 && buffer != NULL)
		{
			const int envPort = std::atoi(buffer);
			std::free(buffer);
			if (envPort > 0 && envPort <= 65535)
			{
				return static_cast<unsigned short>(envPort);
			}
		}

		return defaultPort;
	}

#ifndef BUILD_CONSOLE
	void logServiceError(const char* operation, DWORD error)
	{
		std::cerr << operation << " failed. GetLastError=" << error << std::endl;
	}

	bool startRegisteredService(SC_HANDLE service)
	{
		if (::StartService(service, 0, NULL) != FALSE)
		{
			std::cout << "DongleChecker service started." << std::endl;
			return true;
		}

		const DWORD error = ::GetLastError();
		if (error == ERROR_SERVICE_ALREADY_RUNNING)
		{
			std::cout << "DongleChecker service is already running." << std::endl;
			return true;
		}

		logServiceError("StartService", error);
		return false;
	}

	bool installAndStartService()
	{
		TCHAR executablePath[MAX_PATH] = {};
		if (::GetModuleFileName(NULL, executablePath, MAX_PATH) == 0)
		{
			logServiceError("GetModuleFileName", ::GetLastError());
			return false;
		}

		std::basic_string<TCHAR> binaryPath = TEXT("\"");
		binaryPath += executablePath;
		binaryPath += TEXT("\"");

		SC_HANDLE scm = ::OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
		if (scm == NULL)
		{
			logServiceError("OpenSCManager", ::GetLastError());
			return false;
		}

		SC_HANDLE service = ::OpenService(scm, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
		if (service == NULL)
		{
			const DWORD openError = ::GetLastError();
			if (openError != ERROR_SERVICE_DOES_NOT_EXIST)
			{
				logServiceError("OpenService", openError);
				::CloseServiceHandle(scm);
				return false;
			}

			service = ::CreateService(
				scm,
				SERVICE_NAME,
				SERVICE_NAME,
				SERVICE_START | SERVICE_QUERY_STATUS,
				SERVICE_WIN32_OWN_PROCESS,
				SERVICE_AUTO_START,
				SERVICE_ERROR_NORMAL,
				binaryPath.c_str(),
				NULL,
				NULL,
				NULL,
				NULL,
				NULL);

			if (service == NULL)
			{
				logServiceError("CreateService", ::GetLastError());
				::CloseServiceHandle(scm);
				return false;
			}

			std::cout << "DongleChecker service registered." << std::endl;
		}

		const bool started = startRegisteredService(service);
		::CloseServiceHandle(service);
		::CloseServiceHandle(scm);
		return started;
	}
#endif
}

#ifndef BUILD_CONSOLE
int main(int argc, char* argv[])
{
	SERVICE_TABLE_ENTRY ste[] = {
		{ SERVICE_NAME,(LPSERVICE_MAIN_FUNCTION)MyServiceMain },
		{ NULL,NULL }
	};

	if (::StartServiceCtrlDispatcher(ste) != FALSE)
	{
		return EXIT_SUCCESS;
	}

	const DWORD error = ::GetLastError();
	if (error != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
	{
		logServiceError("StartServiceCtrlDispatcher", error);
		return EXIT_FAILURE;
	}

	return installAndStartService() ? EXIT_SUCCESS : EXIT_FAILURE;
}
#else
int main(int argc, char* argv[])
{
	try
	{
		std::string addressText = "127.0.0.1";
		unsigned short port = getPortFromEnvironment(8082);
		int threads = 1;

		if (argc > 1)
		{
			addressText = argv[1];
		}

		if (argc > 2)
		{
			const int argPort = std::atoi(argv[2]);
			if (argPort > 0 && argPort <= 65535)
			{
				port = static_cast<unsigned short>(argPort);
			}
		}

		if (argc > 3)
		{
			threads = std::max(1, std::atoi(argv[3]));
		}

		const auto address = boost::asio::ip::make_address(addressText);
		boost::asio::io_context ioc(threads);

		std::cout << "DongleChecker listening on " << address << ":" << port << std::endl;
		std::make_shared<Listener>(ioc, tcp::endpoint(address, port))->run();

		std::vector<std::thread> workers;
		workers.reserve(threads > 0 ? threads - 1 : 0);
		for (int i = threads - 1; i > 0; --i)
		{
			workers.emplace_back([&ioc]() {
				ioc.run();
			});
		}

		ioc.run();

		for (size_t i = 0; i < workers.size(); ++i)
		{
			workers[i].join();
		}
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
#endif

void MySetStatus(DWORD dwState, DWORD dwAccept = SERVICE_ACCEPT_STOP |
	SERVICE_ACCEPT_PAUSE_CONTINUE)
{
	SERVICE_STATUS ss;
	ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	ss.dwCurrentState = dwState;
	ss.dwControlsAccepted = dwAccept;
	ss.dwWin32ExitCode = 0;
	ss.dwServiceSpecificExitCode = 0;
	ss.dwCheckPoint = 0;
	ss.dwWaitHint = 0;

	g_NowState = dwState;
	if (g_hSrv != NULL)
	{
		SetServiceStatus(g_hSrv, &ss);
	}

}

void MyServiceMain(DWORD argc, LPTSTR* argv)
{
	ghSvcStopEvent = CreateEvent(
		NULL,    // default security attributes
		TRUE,    // manual reset event
		FALSE,   // not signaled
		NULL);   // no name

	if (ghSvcStopEvent == NULL)
	{
		return;
	}

	g_hSrv = RegisterServiceCtrlHandler(SERVICE_NAME, (LPHANDLER_FUNCTION)MyServiceHandler);
	
	if (g_hSrv == 0) {
		CloseHandle(ghSvcStopEvent);
		ghSvcStopEvent = NULL;
		return;
	}

	MySetStatus(SERVICE_START_PENDING);

	g_bPause = FALSE;

	unsigned short port = getPortFromEnvironment(8082);
	auto const address = boost::asio::ip::make_address("127.0.0.1");
	int threads = 1;

	size_t sz = 0;
	char* buf = nullptr;

	try {
		if (_dupenv_s(&buf, &sz, "HOCT_LICENSE_THREAD") == 0 && buf != nullptr)
		{
			std::cout << "HOCT_LICENSE_THREAD : " << buf << std::endl;

			threads = std::max<int>(threads, static_cast<int>(std::atoi(buf)));
			free(buf);
			buf = nullptr;
		}

	}
	catch (const std::exception& e) {
		std::cout << e.what() << std::endl;
	}
	std::cout << address << std::endl;
	std::cout << port << std::endl;
	std::cout << threads << std::endl;

	boost::asio::io_context ioc{ threads };

	std::make_shared<Listener>(
		ioc,
		tcp::endpoint{ address, port })->run();

	std::thread stopThread([&ioc]() {
		WaitForSingleObject(ghSvcStopEvent, INFINITE);
		ioc.stop();
	});

	std::vector<std::thread> v;
	v.reserve(threads - 1);
	for (auto i = threads - 1; i > 0; --i)
		v.emplace_back(
			[&ioc]
			{
				ioc.run();
			});


	MySetStatus(SERVICE_RUNNING);

	ioc.run();

	if (WaitForSingleObject(ghSvcStopEvent, 0) != WAIT_OBJECT_0)
	{
		SetEvent(ghSvcStopEvent);
	}

	if (stopThread.joinable())
	{
		stopThread.join();
	}

	for (size_t i = 0; i < v.size(); ++i)
	{
		v[i].join();
	}

	MySetStatus(SERVICE_STOPPED);
	CloseHandle(ghSvcStopEvent);
	ghSvcStopEvent = NULL;
}

void MyServiceHandler(DWORD fdwControl)
{
	if (fdwControl == g_NowState)
		return;

	switch (fdwControl) {

	case SERVICE_CONTROL_PAUSE:
		MySetStatus(SERVICE_PAUSE_PENDING, 0);
		g_bPause = TRUE;
		MySetStatus(SERVICE_PAUSED);
		break;

	case SERVICE_CONTROL_CONTINUE:
		MySetStatus(SERVICE_CONTINUE_PENDING, 0);
		g_bPause = FALSE;
		MySetStatus(SERVICE_RUNNING);
		break;

	case SERVICE_CONTROL_STOP:
		MySetStatus(SERVICE_STOP_PENDING, 0);
		if (ghSvcStopEvent != NULL)
		{
			SetEvent(ghSvcStopEvent);
		}
		break;

	case SERVICE_CONTROL_INTERROGATE:
	default:
		MySetStatus(g_NowState);
		break;

	}

}