#ifndef __WSHANDLER__
#define __WSHANDLER__

#include "Application.h"
#include "Config.h"
#include "Log.h"

extern void initSSLContext(boost::asio::ssl::context& context,
	std::string key_file, std::string cert_file, std::string chain_file);

template <typename endpoint_type>
class WSConnection;

// CAUTION: on_* functions are called by the websocket code while holding a lock

// A single instance of this object is made.
// This instance dispatches all events.  There is no per connection persistence.
template <typename endpoint_type>
class WSServerHandler : public endpoint_type::handler
{
public:
	typedef typename endpoint_type::handler::connection_ptr		connection_ptr;
	typedef typename endpoint_type::handler::message_ptr		message_ptr;
	typedef boost::shared_ptr< WSConnection<endpoint_type> >	wsc_ptr;

	// Private reasons to close.
	enum {
		crTooSlow	= 4000,		// Client is too slow.
	};

private:
	boost::shared_ptr<boost::asio::ssl::context>							mCtx;

protected:
	boost::mutex															mMapLock;
	// For each connection maintain an associated object to track subscriptions.
	boost::unordered_map<connection_ptr, boost::shared_ptr< WSConnection<endpoint_type> > >	mMap;
	bool																	mPublic;

public:
	WSServerHandler(boost::shared_ptr<boost::asio::ssl::context> spCtx, bool bPublic) : mCtx(spCtx), mPublic(bPublic)
	{
		if (theConfig.WEBSOCKET_SECURE != 0)
		{
			initSSLContext(*mCtx, theConfig.WEBSOCKET_SSL_KEY,
				theConfig.WEBSOCKET_SSL_CERT, theConfig.WEBSOCKET_SSL_CHAIN);
		}
	}

	bool		getPublic() { return mPublic; };

	void send(connection_ptr cpClient, message_ptr mpMessage)
	{
		try
		{
			cpClient->send(mpMessage->get_payload(), mpMessage->get_opcode());
		}
		catch (...)
		{
			cpClient->close(websocketpp::close::status::value(crTooSlow), std::string("Client is too slow."));
		}
	}

	void send(connection_ptr cpClient, const std::string& strMessage, bool broadcast)
	{
		try
		{
			cLog(broadcast ? lsTRACE : lsDEBUG) << "Ws:: Sending '" << strMessage << "'";

			cpClient->send(strMessage);
		}
		catch (...)
		{
			cpClient->close(websocketpp::close::status::value(crTooSlow), std::string("Client is too slow."));
		}
	}

	void send(connection_ptr cpClient, const Json::Value& jvObj, bool broadcast)
	{
		Json::FastWriter	jfwWriter;

		// cLog(lsDEBUG) << "Ws:: Object '" << jfwWriter.write(jvObj) << "'";

		send(cpClient, jfwWriter.write(jvObj), broadcast);
	}

	void pingTimer(connection_ptr cpClient)
	{
		wsc_ptr ptr;
		{
			boost::mutex::scoped_lock	sl(mMapLock);
			typename boost::unordered_map<connection_ptr, wsc_ptr>::iterator it = mMap.find(cpClient);
			if (it == mMap.end())
				return;
			ptr = it->second;
		}
		std::string data("ping");
		if (ptr->onPingTimer(data))
		{
			cLog(lsWARNING) << "Connection pings out";
			cpClient->close(websocketpp::close::status::PROTOCOL_ERROR, "ping timeout");
		}
		else
			cpClient->ping(data);
	}

	void on_send_empty(connection_ptr cpClient)
	{
		wsc_ptr ptr;
		{
			boost::mutex::scoped_lock	sl(mMapLock);
			typename boost::unordered_map<connection_ptr, wsc_ptr>::iterator it = mMap.find(cpClient);
			if (it == mMap.end())
				return;
			ptr = it->second;
		}

		ptr->onSendEmpty();
	}

	void on_open(connection_ptr cpClient)
	{
		boost::mutex::scoped_lock	sl(mMapLock);

		mMap[cpClient]	= boost::make_shared< WSConnection<endpoint_type> >(this, cpClient);
	}

	void on_pong(connection_ptr cpClient, std::string data)
	{
		wsc_ptr ptr;
		{
			boost::mutex::scoped_lock	sl(mMapLock);
			typename boost::unordered_map<connection_ptr, wsc_ptr>::iterator it = mMap.find(cpClient);
			if (it == mMap.end())
				return;
			ptr = it->second;
		}
		ptr->onPong(data);
	}

	void on_close(connection_ptr cpClient)
	{ // we cannot destroy the connection while holding the map lock or we deadlock with pubLedger
		wsc_ptr ptr;
		{
			boost::mutex::scoped_lock	sl(mMapLock);
			typename boost::unordered_map<connection_ptr, wsc_ptr>::iterator it = mMap.find(cpClient);
			if (it == mMap.end())
				return;
			ptr = it->second;		// prevent the WSConnection from being destroyed until we release the lock
			mMap.erase(it);
		}
		ptr->preDestroy(); // Must be done before we return

		// Must be done without holding the websocket send lock
		theApp->getJobQueue().addJob(jtCLIENT, "WSClient::destroy",
			boost::bind(&WSConnection<endpoint_type>::destroy, ptr));
	}

	void on_message(connection_ptr cpClient, message_ptr mpMessage)
	{
		theApp->getJobQueue().addJob(jtCLIENT, "WSClient::command",
			boost::bind(&WSServerHandler<endpoint_type>::do_message, this, _1, cpClient, mpMessage));
	}

	void do_message(Job& job, connection_ptr cpClient, message_ptr mpMessage)
	{
		Json::Value		jvRequest;
		Json::Reader	jrReader;

	    cLog(lsDEBUG) << "Ws:: Receiving("
	    	<< cpClient->get_socket().lowest_layer().remote_endpoint().address().to_string()
			<< ") '" << mpMessage->get_payload() << "'";

		if (mpMessage->get_opcode() != websocketpp::frame::opcode::TEXT)
		{
			Json::Value	jvResult(Json::objectValue);

			jvResult["type"]	= "error";
			jvResult["error"]	= "wsTextRequired";	// We only accept text messages.

			send(cpClient, jvResult, false);
		}
		else if (!jrReader.parse(mpMessage->get_payload(), jvRequest) || jvRequest.isNull() || !jvRequest.isObject())
		{
			Json::Value	jvResult(Json::objectValue);

			jvResult["type"]	= "error";
			jvResult["error"]	= "jsonInvalid";	// Received invalid json.
			jvResult["value"]	= mpMessage->get_payload();

			send(cpClient, jvResult, false);
		}
		else
		{
			if (jvRequest.isMember("command"))
				job.rename(std::string("WSClient::") + jvRequest["command"].asString());
			boost::shared_ptr< WSConnection<endpoint_type> > conn;
			{
				boost::mutex::scoped_lock	sl(mMapLock);
				typedef boost::shared_ptr< WSConnection<endpoint_type> > wsc_ptr;
				typename boost::unordered_map<connection_ptr, wsc_ptr>::iterator it = mMap.find(cpClient);
				if (it == mMap.end())
					return;
				conn = it->second;
			}
			send(cpClient, conn->invokeCommand(jvRequest), false);
		}
	}

	boost::shared_ptr<boost::asio::ssl::context> on_tls_init()
	{
		return mCtx;
	}

	// Respond to http requests.
	void http(connection_ptr cpClient)
	{
		cpClient->set_body(
			"<!DOCTYPE html><html><head><title>" SYSTEM_NAME " Test</title></head>"
			"<body><h1>" SYSTEM_NAME " Test</h1><p>This page shows http(s) connectivity is working.</p></body></html>");
	}
};

#endif

// vim:ts=4
