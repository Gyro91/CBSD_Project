/**
 *	server_class.cpp
 * 
 */
#include <iostream>
#include <sstream> 
#include <stdio.h>
#include <time.h>
#include <ctime>
#include <thread>
#include "../../include/server_class.hpp"
#include "../../include/communication.hpp"
#include "../../include/test.hpp"
#include "../../include/util.hpp"

#define MAX_LENGTH_STRING_PORT 6 /* Max number of char needed for data port */

/**
 * @brief Server constructor that initializes alle the private data and
 * 	  claims memory for ZMQ sockets. Then it connects to the socket.
 * @param id_server Identifier among server copies
 * @param service_t Service type to be deployed
 * @param server_p Server receive port
 * @param broker_addr Broker address
 * @param broker_port Broker registration port
 * 
 */

RSF_Server::RSF_Server(uint8_t id, uint8_t service_type, 
	std::string broker_address, uint16_t broker_port) 
{
	this->id = id;
	this->service_type = (service_type_t)service_type;
	this->broker_address = broker_address;
	this->service = get_service_body(this->service_type);
	this->service_thread.service = service;
	this->service_thread.service_type = this->service_type;
	this->service_thread.id = id;
	this->ping_id = 0;
	this->request_id = 0;
	
	/* Allocating ZMQ context */
	try {
		context = new zmq::context_t(1);
	} catch (std::bad_alloc& ba) {
		std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
		exit(EXIT_FAILURE);
	}

	try {
		registrator = new Registrator(broker_address, 
			(service_type_t) service_type, broker_port, 
			context);
	} catch (std::bad_alloc& ba) {
		std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
		exit(EXIT_FAILURE);
	}

	/* Add the pong socket */
	hc_pong = add_socket(context, ANY_ADDRESS, SERVER_PONG_PORT + id +
		service_type * MAX_NMR, ZMQ_REP, BIND);
	my_name = "Server" + std::to_string((int32_t)id);
}

/**
 * @brief Server denstructor to destroy all the dynamic objects
 * 
 */

RSF_Server::~RSF_Server()
{
	delete context;
	delete reply;
	delete registrator;
}

/**
 * @brief Server step function used to perform all the computation.
 */

void RSF_Server::step()
{	 
	uint32_t received_id;
	char_t val[PARAM_SIZE];
	int32_t ping_loss = 0;
	struct timespec tmp_t, time_t;
	bool heartbeat, reg_ok = false;
	
	server_reply_t server_reply;

	/* Adding the sockets to the poll set */
	zmq::pollitem_t item = {static_cast<void*>(*hc_pong), 0, ZMQ_POLLIN, 0};
	items.push_back(item);;
	
	for (;;) {
		zmq::poll(items, 0);
		clock_gettime(CLOCK_MONOTONIC, &tmp_t);
		
		/* Check for a service request */
		if (reg_ok && (items[SERVICE_REQUEST_INDEX].revents 
			& ZMQ_POLLIN)) {
			heartbeat = receive_request(val, &received_id);
			clock_gettime(CLOCK_MONOTONIC, &time_t);
			time_add_ms(&time_t, 
					HEARTBEAT_INTERVAL + WCDPING);
			if (!heartbeat) {
				if (request_id == 0)
					request_id = received_id;
				write_log(my_name, "Received request " + 
				std::to_string(received_id) + " expected " +
				std::to_string(request_id));
				if (received_id == request_id) {
					request_id++;
					/* Spawning a thread to service 
					 * the request */
					create_thread(val);
				} else {
					zmq::message_t msg(
						sizeof(server_reply_t));
					server_reply.id = id;
					server_reply.heartbeat = false;
					server_reply.service = (service_type_t)
						htonl((uint32_t) service_type);
					server_reply.duplicated = true;
					memcpy(msg.data(), (void*) 
						&server_reply, 
						sizeof(server_reply_t));
					reply->send(msg);
				}

			} else {
				if (ping_id == 0)
					ping_id = received_id;
				write_log(my_name, "Received ping " + 
				std::to_string(received_id) + " expected " +
				std::to_string(ping_id));
				if (received_id == ping_id) 
					ping_id++;
				
				write_log(my_name, "Send pong " + 
					std::to_string(ping_id) + " to Broker");
				pong_broker();
			}
		}
		
		if (items[SERVER_PONG_INDEX].revents & ZMQ_POLLIN) {
			/* Receive the ping from the health checker */
			write_log(my_name, "Received ping from HC");
			pong_health_checker();
		}
		
		if (!reg_ok) 
			{
			/* Add the reply socket */
			this->broker_port = registrator->registration();
			if (this->broker_port > 0 && this->broker_port <= 65535) 
				{
				write_log(my_name, "Registration Ok!" 
					 " Received port " + 
					std::to_string(this->broker_port));
				/* In this case the REP socket requires 
				 * the connect() method! */
				delete reply;
				reply = add_socket(context, broker_address, 
				broker_port, ZMQ_REP, CONNECT);
				item = {static_cast<void*>(*reply), 0, 
					ZMQ_POLLIN, 0};
				items.push_back(item);
				reg_ok = true;
				ping_loss = 0;
				clock_gettime(CLOCK_MONOTONIC, &time_t);
				time_add_ms(&time_t, 
					HEARTBEAT_INTERVAL + WCDPING);

			} else if (this->broker_port == 0) {
				std::cerr << "Error in the registration!" << 
				std::endl;
				exit(EXIT_FAILURE);
			} else if (this->broker_port == -1) {
				std::cerr << "timeout receive expired" << 
				std::endl;
			}
		}
 
		if (time_cmp(&tmp_t, &time_t) == 1 && reg_ok) {
			write_log(my_name, "Broker ping timeout");
			clock_gettime(CLOCK_MONOTONIC, &time_t);
				time_add_ms(&time_t, 
				HEARTBEAT_INTERVAL + WCDPING);
			/* Timeout expired. It is a Ping loss from the broker */
			if (++ping_loss == LIVENESS) {
				write_log(my_name, "Broker dead");
				items.erase(items.end() - 1);
				reg_ok = false;
			}
		}
	}
}

/**
 * @brief Receive the request message from the broker and returns 
 * 	  the data contained inside it.
 * @param val	Reference to return the message data.
 * @param received_id Where to put the seq id of the received message
 * 
 * @return it returns true if it is a broker ping
 */

bool RSF_Server::receive_request(char_t *val, uint32_t* received_id)
{
	zmq::message_t msg;
	service_module sm;
	
	reply->recv(&msg);
	sm = *(static_cast<service_module *> (msg.data()));

	if (sm.heartbeat == false) {
		memcpy(val, sm.parameters, sizeof(sm.parameters));
		std::string str(val);
		write_log(my_name, "Received parameters " + str);
	}

	*received_id = ntohl(sm.seq_id);

	return sm.heartbeat;
}


/**
 * @brief It sends a pong to the broker 
 */
 
void RSF_Server::pong_broker()
{
	server_reply_t server_reply;
	zmq::message_t msg(sizeof(server_reply_t));
	
	server_reply.id = id; /* Pong from server id */
	server_reply.service = (service_type_t) htonl((uint32_t) service_type);
	server_reply.heartbeat = true;
	
	memcpy(msg.data(), (void *) &server_reply, sizeof(server_reply_t));
	reply->send(msg);
}

/**
 * @brief Sends a pong message to the health checker
 */

void RSF_Server::pong_health_checker()
{
	zmq::message_t msg;
	
	/* Receive the ping */
	hc_pong->recv(&msg);
	msg.rebuild(EMPTY_MSG, 0);
	
	/* Send the pong */
	hc_pong->send(msg);
}

void task(service_thread_t st) 
{	
	int32_t val_elab;
	int32_t par1;
	server_reply_t server_reply;
	zmq::message_t msg(sizeof(server_reply_t));
	std::stringstream par_stream(st.parameters, std::ios_base::in);
	
	/* Simulate workload */
	busy_wait(500);

	deserialize(par_stream, par1);
	
	val_elab = st.service(par1);
	
	server_reply.id = st.id;
	server_reply.heartbeat = false;
	server_reply.result = (int32_t) htonl(val_elab);
	server_reply.service = (service_type_t) htonl((uint32_t)
		st.service_type);
	server_reply.duplicated = false;
	
	memcpy(msg.data(), (void *) &server_reply, sizeof(server_reply_t));
	st.skt->send(msg);
}

/**
 * @brief Spawns a thread to handle the request
 * @param parameters Service parameters
 */

void RSF_Server::create_thread(std::string parameters)
{
	service_thread.skt = reply;
	service_thread.parameters = parameters;
	
	std::thread(task, service_thread).detach();
}

