/*
 * broker.cpp
 * Main program of the broker
 */

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include "../../include/types.hpp"
#include "../../include/broker_class.hpp"
#include "../../include/communication.hpp"
#include "../../include/service_database_class.hpp"
#include <zmq.hpp>


int32_t main(int32_t argc, char_t* argv[])
{	
	/*
	registration_module rm;
	rm.signature = "pippo";
	rm.service = INCREMENT;

	ServiceDatabase db = ServiceDatabase();
	db.push_registration(&rm);
	db.push_registration(&rm);

	rm.signature = "ezio";
	rm.service = DECREMENT;
	db.push_registration(&rm);
	db.print_htable();
	*/
	
	//Broker broker(3, ROUTER_PORT_BROKER, REG_PORT_BROKER);

	//broker.step();

	return EXIT_SUCCESS;
}