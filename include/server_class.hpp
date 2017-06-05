/*
 *	server_class.hpp
 *
 */

#ifndef INCLUDE_SERVER_CLASS_HPP_
#define INCLUDE_SERVER_CLASS_HPP_

#include <zmq.hpp>
#include <string>
#include <unistd.h>
#include "types.hpp"
#include "service.hpp"
#include "registrator_class.hpp"

#define SERVICE_REQUEST_INDEX 1
#define REGISTRATION_INDEX 1
#define SERVER_PONG_INDEX 0

struct service_thread_t {
	int32_t parameter;
	service_body service;
	service_type_t service_type;
	uint8_t id;
	zmq::socket_t *skt;
};

class Server {

private:
	/* Idientifier among server copies */
	uint8_t id;
	/* Service type that must be provided */
	service_type_t service_type;
	/* Service to be provided */
	service_body service;
	/* Ping seq id */
	uint64_t ping_id;
	/* Address and port for communication */
	std::string broker_address;
	int32_t broker_port;
	/* Sockets for ZMQ communication */
	zmq::context_t *context;
	zmq::socket_t *reply;
	zmq::socket_t *hc_pong;
	service_thread_t service_thread;
	/* Registrator to register this unit to the broker */
	Registrator *registrator;
	/* Poll set */
	std::vector<zmq::pollitem_t> items;
	
	/* Receive requests from the broker */
	bool receive_request(int32_t *val, uint64_t *received_id);
	/* Send results to the broker */
	void deliver_service(int32_t val);
	/* Send a pong to the broker */
	void pong_broker();
	/* Receive the ping and send back a pong to the health checker */
	void pong_health_checker();
	/* Function for creating a thread that elaborates a request */
	void create_thread(int32_t parameter);

public:
	Server(uint8_t id, uint8_t service, std::string broker_addr);
	void step();
	~Server();
};

#endif /* INCLUDE_SERVER_CLASS_HPP_ */