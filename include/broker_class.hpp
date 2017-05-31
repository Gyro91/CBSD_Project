/*
 * broker_class.hpp
 *
 */

#ifndef INCLUDE_BROKER_CLASS_HPP_
#define INCLUDE_BROKER_CLASS_HPP_

#include <zmq.hpp>
#include <string>
#include <unistd.h>
#include <list>
#include "types.hpp"
#include "service.hpp"
#include "service_database_class.hpp"

#define ROUTER_POLL_INDEX 0
#define REG_POLL_INDEX 1
#define HC_POLL_INDEX 2
#define DEALER_POLL_INDEX 3

/**
 * @class Broker
 * @file broker_class.hpp
 * @brief 
 */
 
class Broker {

private:
	/* Redundancy for the voter */
	uint8_t nmr;
	/* Ports for communication */
	std::list<uint16_t> port_dealer; 
	uint16_t port_router;
	uint16_t port_reg;
	uint16_t available_dealer_port;
	/* Poll set */
	std::vector<zmq::pollitem_t> items;
	/* Sockets for ZMQ communication */
	zmq::context_t *context;
	std::vector<zmq::socket_t*> dealer;
	zmq::socket_t *reg;
	zmq::socket_t *router;
	zmq::socket_t *hc;
	/* Services Database */
	ServiceDatabase *db;
	/* Vector of available services */
	std::vector<service_type_t> available_services;
	
	/* Function for voting */
	uint8_t vote(std::vector<int32_t> values, int32_t &result);
	/* Function for sending a ping to all the servers */
	void ping_servers();
	/* Function for adding a dealer socket */
	void add_dealer(uint16_t dealer_port);
	/* Function to get a request from the client */
	void get_request();
	/* Function to get a registration from the server */
	void get_registration();
	/* Function to get a service response from a server */
	void get_response(uint32_t dealer_index);
	/* Function for printing the available services */
	void print_available_services();
	/* Function for sending a pong to the health checker */
	void pong_health_checker();
public:
	Broker(uint8_t nmr, uint16_t port_router, uint16_t port_reg);
	void step();
	~Broker();
};

#endif /* INCLUDE_BROKER_CLASS_HPP_ */