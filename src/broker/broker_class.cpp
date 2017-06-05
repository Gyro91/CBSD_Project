/*
 *	broker_class.cpp
 *
 */
#include <string>
#include <iostream>
#include <unistd.h>
#include "../../include/broker_class.hpp"
#include "../../include/communication.hpp"
#include "../../include/util.hpp"
#include "../../include/rs_api.hpp"

/**
 * @brief Broker constructor that initializes alle the private data and
 * 	  claims memory for ZPQ sockets. Then it connects to the socket.
 * @param nmr Redundancy for the voter
 * @param port_router It is the port for client communication
 * @param port_reg It is the port for the server registration
 * 
 */

Broker::Broker(uint8_t nmr, uint16_t port_router, uint16_t port_reg) 
{	
	int32_t opt;

	this->nmr = nmr;
	this->port_router = port_router;
	this->port_reg = port_reg;
	this->available_dealer_port = DEALER_START_PORT;

	/* Allocating ZMQ context */
	try {
		context = new zmq::context_t(1);
	} catch (std::bad_alloc& ba) {
		std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
		exit(EXIT_FAILURE);
	}

	/* Router socket creation */
	router = add_socket(context, ANY_ADDRESS, port_router, ZMQ_ROUTER, 
		BIND);
	/* This option is used to enable error messages when an invalid
	 * identity is used to send a message with a ROUTER socket
	 */
	opt = 1;
	router->setsockopt(ZMQ_ROUTER_MANDATORY, &opt, sizeof(int32_t));
	
	/* Registration socket creation */
	reg = add_socket(context, ANY_ADDRESS, port_reg, ZMQ_ROUTER, BIND);
	/* Health checker socket creation */
	hc = add_socket(context, ANY_ADDRESS, BROKER_PONG_PORT, ZMQ_REP, BIND);
	/* Initialize the poll set */
	zmq::pollitem_t tmp = {static_cast<void*>(*router), 0, ZMQ_POLLIN, 0};
	items.push_back(tmp);
	tmp = {static_cast<void*>(*reg), 0, ZMQ_POLLIN, 0};
	items.push_back(tmp);
	tmp = {static_cast<void*>(*hc), 0, ZMQ_POLLIN, 0};
	items.push_back(tmp);

	/* Creating a Service Database*/
	db = new ServiceDatabase(nmr);
}



/**
 * @brief      Destroys the object.
 */

Broker::~Broker()
{
	delete router;
	delete reg;
	delete db;
	delete context;
}

/**
 * @brief      step function
 */

void Broker::step()
{		
	for (;;) {
		zmq::poll(items, HEARTBEAT_INTERVAL);
		clock_gettime(CLOCK_MONOTONIC, &now);

		
		/* Check the ping from the health checker*/
		if (items[HC_POLL_INDEX].revents & ZMQ_POLLIN) {
			std::cout << "Broker: Received ping from HC" 
				<< std::endl;
			pong_health_checker();	
		}
		/* Check for a registration request */
		if (items[REG_POLL_INDEX].revents & ZMQ_POLLIN) 
			get_registration();			
	
		/* Check for messages on the ROUTER socket */
		if (items[ROUTER_POLL_INDEX].revents & ZMQ_POLLIN) 
			get_request();		
		
		/* Check for messages on the DEALER sockets */
		for (uint32_t i = 0; i < dealer.size(); i++) 
			if (items[i + DEALER_POLL_INDEX].revents & 
				ZMQ_POLLIN) 
				get_response(i);	
		
		for (uint32_t i = 0; i < timeout.size(); i++) {
			if (time_cmp(&now, &timeout[i]) == 1) {
				std::cout << "Heartbeat" << std::endl;
				db->check_pong(available_services[i]);
				ping_server(i, available_services[i]);
				update_timeout(available_services[i]);
			}
		}
	}
}

/**
 * @brief      Adds a dealer.
 *
 * @param[in]  dealer_port  The dealer port
 */

void Broker::add_dealer(uint16_t dealer_port)
{	
	zmq::pollitem_t item;

	dealer.push_back(add_socket(context, ANY_ADDRESS, dealer_port, 
		ZMQ_DEALER, BIND));
	
	item = {static_cast<void*>(*dealer.back()), 0, ZMQ_POLLIN, 0};
	
	items.push_back(item);
}


/**
 * @brief      Gets the request from a client 
 */
 
void Broker::get_request()
{	
	int32_t ret;
	request_module request;
	response_module response;
	request_record_t request_record;
	service_module sm;
	std::vector<zmq::message_t> buffer_in(NUM_FRAMES);

	/* Receive multiple messages,
	 * one frame at a time */
	for (uint8_t i = 0; i < ENVELOPE; i++) {
		router->recv(&buffer_in[i]);
		if (i == ID_FRAME) {
			/* We get the client id and we add a request record */
			uint8_t *p = (uint8_t *) buffer_in[i].data();
			p++;
			request_record.client_id = *((uint32_t *) p);
		}
	}

	request = *(static_cast<request_module*> (buffer_in[DATA_FRAME].data())); 
	ret = db->find_registration(request.service);
	if (ret == -1) {
		/* Service not available */
		response.service_status = SERVICE_NOT_AVAILABLE;
		buffer_in[DATA_FRAME].rebuild((void*) &response,
			sizeof(response_module));
		send_multi_msg(router, buffer_in);

	} else {
		/* Service available */
		sm.heartbeat = false;
		sm.parameter = request.parameter;
		buffer_in[DATA_FRAME].rebuild((void*) &sm,
			sizeof(service_module));
		/* Forwarding the parameter */
		for (uint8_t j = 0; j < nmr; j++)
			send_multi_msg(dealer[ret], buffer_in);
		/* Saving the request in the db */
		db->push_request(&request_record, request.service);
		/* Postponing timeout */
		update_timeout(request.service);
	}
}

/**
 * @brief      Gets the registration from a server.
 */

void Broker::get_registration()
{	
	int32_t more;
	size_t more_size;
	bool already_registered = false, ready = false;
	zmq::message_t message;

	for (uint8_t i = 0; i < ENVELOPE; i++) {
		reg->recv(&message);
		more_size = sizeof(more);
		reg->getsockopt(ZMQ_RCVMORE, &more, 
			&more_size);

		if (more == 1) 
			reg->send(message, ZMQ_SNDMORE);
		if (!more) {
			/* Receiving the registration module */
			std::cout << "Receiving registration" 
			<< std::endl;
			registration_module rm = 
			*(static_cast<registration_module*> 
				(message.data()));

			/* Registering */
			uint16_t ret = 
			db->push_registration(&rm, available_dealer_port, 
				ready);
			
			for (uint32_t i = 0; i < available_services.size(); i++)
				if (available_services[i] == rm.service)
					already_registered = true;
			/* If all the copies are registered */
			if (ready && !already_registered) {
				/* Make the service available */
				available_services.push_back(rm.service);
				/* Add a dealer port */
				add_dealer(ret);
				/* Setting the timeout for the copies */
				struct timespec timeout_tmp;
				clock_gettime(CLOCK_MONOTONIC, &timeout_tmp);
				time_add_ms(&timeout_tmp, HEARTBEAT_INTERVAL);
				timeout.push_back(timeout_tmp);
			}
			db->print_htable();
			print_available_services();
			/* Sending back the dealer port */
			zmq::message_t reply(sizeof(ret));
			memcpy(reply.data(), 
				(void *) &ret, sizeof(ret));
			reg->send(reply, 0);
		}
	}
	
}

/**
 * @brief      Gets the response from a server
 *
 * @param[in]  dealer_index  The dealer index
 * @param      num_replies   the replies number for the service
 * @param      dealer_in     The dealer buffer
 */

void Broker::get_response(uint32_t dealer_index)
{	
	int32_t num_copies, ret, result;
	uint32_t i = 0;
	server_reply_t server_reply;
	response_module response;
	zmq::message_t message;
	uint32_t client_id;
	std::vector<zmq::message_t> buffer_in(NUM_FRAMES);

	/* Receiving all the messages */

	for (i = 0; i < ENVELOPE; i++) {
		dealer[dealer_index]->recv(&buffer_in[i], ZMQ_DONTWAIT);
		if (i == ID_FRAME) {
			uint8_t *p = (uint8_t *) buffer_in[i].data();
			p++;
			client_id = *((uint32_t *) p);
		}			
	}
	
	server_reply = *(static_cast<server_reply_t*>
			(buffer_in[DATA_FRAME].data()));
			
	if (server_reply.id >= 0) {
		std::cout << "Pong from Server" << (int32_t) server_reply.id <<
		" service " << server_reply.service << std::endl;
		db->register_pong(server_reply.id, server_reply.service);
	} else {
		num_copies = db->push_result(&server_reply, client_id);
		if (num_copies == nmr) {
			ret = vote(db->get_result(server_reply.service, 
				client_id), result);
			if (ret >= 0) {
				/* Replace the data frame with the
				 * one obtained from the voter.
				 */
				response.service_status = SERVICE_AVAILABLE;
				response.result = result;
				buffer_in[DATA_FRAME].rebuild((void*) &response, 
				sizeof(response_module));
				send_multi_msg(router, buffer_in);
			}

			/* Deleting service request */
			db->delete_request(server_reply.service, client_id);
		}
	}
}

/*
 * @brief Implements the voting logic
 * @param values List containing the values returned from the servers
 * @return >0 index of the majority value, -1 there is no majority
 */

uint8_t Broker::vote(std::vector<int32_t> values, int32_t &result)
{	
	if (values[0] == values[1] || values[0] == values [2]) {
		result = values[0];
		return 0;
	}
	else if (values[1] == values[2]) {
		result = values[1];
		return 0;
	}
	else
		return -1;
}

/**
 * @brief It sends a ping to all the servers
 */
 
void Broker::ping_servers()
{
	service_module sm;
	uint8_t num_copies_reliable;
	std::vector<zmq::message_t> buffer_in(NUM_FRAMES);
	char_t address_ping[LENGTH_ID_FRAME];
	
	address_ping[0] = 0;
	memset((address_ping + 1), 'a', LENGTH_ID_FRAME - 1);
	
	/* In order to reuse the same dealer port for receiving pong and
	 * results we have to emulate the router-dealer-rep pattern */
	sm.heartbeat = true;
	for (uint32_t i = 0; i < dealer.size(); i++) {
		buffer_in[ID_FRAME].rebuild((void*) &address_ping[0], 
			sizeof(address_ping));
		buffer_in[EMPTY_FRAME].rebuild((void*) "", 0);
		buffer_in[DATA_FRAME].rebuild((void*) &sm,
			sizeof(service_module));
		num_copies_reliable = 
			db->get_reliable_copies(available_services[i]);
		
		for(uint8_t j = 0; j < num_copies_reliable; j++) 
			send_multi_msg(dealer[i], buffer_in);
	}
}

/**
 * @brief It sends a ping to a specific group of servers
 * @param i index to the servers group
 * @param service service type
 */

void Broker::ping_server(uint8_t i, service_type_t service)
{
	service_module sm;
	uint8_t num_copies_reliable;
	std::vector<zmq::message_t> buffer_in(NUM_FRAMES);
	char_t address_ping[LENGTH_ID_FRAME];
	
	address_ping[0] = 0;
	memset((address_ping + 1), 'a', LENGTH_ID_FRAME - 1);
	
	/* In order to reuse the same dealer port for receiving pong and
	 * results we have to emulate the router-dealer-rep pattern */
	sm.heartbeat = true;
	sm.seq_id = db->get_ping_id(service);
	buffer_in[ID_FRAME].rebuild((void*) &address_ping[0], 
		sizeof(address_ping));
	buffer_in[EMPTY_FRAME].rebuild((void*) "", 0);
	buffer_in[DATA_FRAME].rebuild((void*) &sm, sizeof(service_module));
	num_copies_reliable = db->get_reliable_copies(available_services[i]);
	
	for(uint8_t j = 0; j < num_copies_reliable; j++) {
		send_multi_msg(dealer[i], buffer_in);
		std::cout<< "Sending message " << sm.seq_id << " to Server " <<
		(int32_t)j << std::endl;
	}
}

/**
 * @brief It prints all the available services 
 */
 
void Broker::print_available_services()
{	
	for (uint32_t i = 0; i < available_services.size(); i++)
		std::cout << "Service " << available_services[i] << " " << 
		std::endl;
}

void Broker::pong_health_checker()
{
	zmq::message_t msg;
	
	/* Receive the ping */
	hc->recv(&msg);
	msg.rebuild(EMPTY_MSG, 0);
	
	/* Send the pong */
	hc->send(msg);
}

void Broker::update_timeout(service_type_t service)
{
	uint32_t i;
	struct timespec timeout_tmp;
	
	for (i = 0; i < available_services.size(); i++)
		if (available_services[i] == service)
			break;
	
	clock_gettime(CLOCK_MONOTONIC, &timeout_tmp);
	time_add_ms(&timeout_tmp, HEARTBEAT_INTERVAL);
	timeout[i] = timeout_tmp;
}