#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>  // htons() and inet_addr()
#include <netinet/in.h> // struct sockaddr_in
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include "common.h"
#include "image.h"
#include "surface.h"
#include "world.h"
#include "vehicle.h"
#include "world_viewer.h"
#include "so_game_protocol.h"
#include "user_list.h"

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~GLOBALS~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

#define GOOD_NIGHT   10000         // Imposta timeout di aggiornamento

// Struttura per args dei threads in TCP
typedef struct {
  int client_desc;
  struct sockaddr_in client_addr;
  Image* elevation_texture;
  Image* surface_elevation;
} tcp_args_t;

// To avoid race condition I need a mutex to lock the most important events in the code
pthread_mutex_t mutex;

World world;
UserHead* users;

// "Boolean", it will ensure me the game status
int running;

// Stuff needed for the connection between client and server
int tcp_socket, udp_socket;
pthread_t TCP_connection, UDP_sender_thread, UDP_receiver_thread;

//These two are the info related to the map in which the vehicles will go around
Image* surface_elevation;
Image* surface_texture;

/* Freeuping the memory*/
void freeMemory(void) {
  int ret;
  printf("Free Memory: Preparing to deallocate everything I used\n");
  running = 0;
  ret = close(tcp_socket);
  ERROR_HELPER(ret, "Cannot close TCP socket");
  ret = close(udp_socket);
  ERROR_HELPER(ret, "Cannot close UDP socket");
  ret = pthread_cancel(UDP_sender_thread);
  ERROR_HELPER(ret, "Cannot cancel the UDP sender thread");
  ret = pthread_cancel(UDP_receiver_thread);
  ERROR_HELPER(ret, "Cannot cancel the UDP receiver thread");
  ret = pthread_cancel(TCP_connection);
  ERROR_HELPER(ret, "Cannot cancel the TCP connection thread");

  World_destroy(&world);
  Image_free(surface_elevation);
  Image_free(surface_texture);

  printf("Free Memory: Memory cleaned...\n");
  return;
}
    
/* Funzione per la gestione dei segnali */
void signalHandler(int signal){
  switch (signal) {
	case SIGHUP:
	  printf("\nSighup occured, switching off\n");	
	  break;
	case SIGINT:
	  printf("\nSigInt occured,switching off\n");
	  break;
	case SIGTERM:
      printf("\nSigTerm occured,switching off\n");
      break;
	//~ default:
	  //~ printf("\nUncaught signal: %d\n", signal);
	  //~ return;
  }
  freeMemory();
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~TCP PART~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void tcp_initialization(void){
  
  int ret;  
  tcp_socket = socket(AF_INET , SOCK_STREAM , 0);
  ERROR_HELPER(tcp_socket, "[TCP] Failed to create TCP socket");

  struct sockaddr_in tcp_server_addr = {0};
  int sockaddr_len = sizeof(struct sockaddr_in);
  tcp_server_addr.sin_addr.s_addr = INADDR_ANY;
  tcp_server_addr.sin_family      = AF_INET;
  tcp_server_addr.sin_port        = htons(TCP_PORT);

  int reuseaddr_opt_tcp = 1;
  ret = setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_opt_tcp, sizeof(reuseaddr_opt_tcp));
  ERROR_HELPER(ret, "[ERROR] Failed setsockopt on TCP server socket!!!");
  
  ret = bind(tcp_socket, (struct sockaddr*) &tcp_server_addr, sockaddr_len);
  ERROR_HELPER(ret, "[ERROR] Failed bind address on TCP server socket!!!");

  ret = listen(tcp_socket, 3);
  ERROR_HELPER(ret, "[ERROR] Failed listen on TCP server socket!!!");
  if (ret>=0)
    printf("%sServer listening on port %d...\n",TCP, TCP_PORT);
}

int TCP_packet (int client_tcp_socket, int id, char* buffer, Image* surface_elevation, Image* elevation_texture, int len, User* user) {
  PacketHeader* header = (PacketHeader*) buffer;  // Pacchetto per controllo del tipo di richiesta

  // What kind of packet have I received? Let's see:
  
  //A client has successfully answered me, he had received the new client texture
  if (header->type == NewClient){
  	printf("%sClient has been notified\n",TCP);
  	return 1;
  }

  //The client requires an ID
  if (header->type == GetId) {

    printf("%sID requested:%d\n",TCP, id);

    // I'll create an ID packet to send it through serialization in a buffer
    IdPacket* id_to_send = (IdPacket*) malloc(sizeof(IdPacket));

    PacketHeader header_send;
    header_send.type = GetId;
    
    id_to_send->header = header_send;
    id_to_send->id = id;  // The same TCPHandler gave it

    char buffer_send[BUFFER_SIZE];
    int pckt_length = Packet_serialize(buffer_send, &(id_to_send->header));

    // Sending the message
    int bytes_sent = 0;
    int ret;
    while(bytes_sent < pckt_length){
      ret = send(client_tcp_socket, buffer_send + bytes_sent, pckt_length - bytes_sent,0);
      if (ret==-1 && errno==EINTR) continue;
      ERROR_HELPER(ret, "[ERROR] Error assigning ID!!!");
      if (ret==0) break;
      bytes_sent += ret;
    }

    printf("%sID assigned to %d\n",TCP, id);

    return 1;
  }

  // The client requires the map texture
  else if (header->type == GetTexture) {

    printf("%sUser %d requested the map texture\n",TCP, id);

    //Converting received packet into an ImagePacket to extract the texture the client wants
    
    ImagePacket* texture_request = (ImagePacket*) buffer;
    int id_request = texture_request->id;
    
    // I'm now preparing the header for the response
    PacketHeader header_send;
    header_send.type = PostTexture;
 
    // Preparing the packet to answer the client with the texture he wants
    ImagePacket* texture_to_send = (ImagePacket*) malloc(sizeof(ImagePacket));
    texture_to_send->header = header_send;
    texture_to_send->id = id_request;
    texture_to_send->image = elevation_texture;

    char buffer_send[BUFFER_SIZE];
    int pckt_length = Packet_serialize(buffer_send, &(texture_to_send->header));

    // Sending through socket
    int bytes_sent = 0;
    int ret;
    while(bytes_sent < pckt_length){
      ret = send(client_tcp_socket, buffer_send + bytes_sent, pckt_length - bytes_sent,0);
      if (ret==-1 && errno==EINTR) continue;
      ERROR_HELPER(ret, "There's been an error requesting the mmap texture");
      if (ret==0) break;
      bytes_sent += ret;
    }

    printf("%sTexture sent successfully to %d\n",TCP, id);   // DEBUG OUTPUT

    return 1;
  }

  // The client requires the elevation texture
  else if (header->type == GetElevation) {

    printf("%sClient %d has requested the elevation map\n",TCP, id);

   //Converting received packet into an ImagePacket to extract the texture the client wants
    ImagePacket* elevation_request = (ImagePacket*) buffer;
    int id_request = elevation_request->id;
    
    // I'm now preparing the header for the response
    PacketHeader header_send;
    header_send.type = PostElevation;
    
    // Preparing the packet to answer the client with the elevation texture he wants
    ImagePacket* elevation_to_send = (ImagePacket*) malloc(sizeof(ImagePacket));
    elevation_to_send->header = header_send;
    elevation_to_send->id = id_request;
    elevation_to_send->image = surface_elevation;

    char buffer_send[BUFFER_SIZE];
    int pckt_length = Packet_serialize(buffer_send, &(elevation_to_send->header));

    // Sending through socket
    int bytes_sent = 0;
    int ret;
    while(bytes_sent < pckt_length){
      ret = send(client_tcp_socket, buffer_send + bytes_sent, pckt_length - bytes_sent,0);
      if (ret==-1 && errno==EINTR) continue;
      ERROR_HELPER(ret, "Error elevation texture!!!");
      if (ret==0) break;
      bytes_sent += ret;
    }

    printf("%sElevation map sent successfully to %d\n",TCP, id);

    return 1;
  }

  // I'm receiving a texture from the client
  else if (header->type == PostTexture) {
    int ret;
 
 	// The packet I received is not complete
    if (len < header->size) return -1;

    // Deserialization of the packet
    PacketHeader* received_header = Packet_deserialize(buffer, header->size);
    ImagePacket* received_texture = (ImagePacket*) received_header;

    printf("%sI received the Vehicle Texture, ID: %d\n",TCP, id);
    
    // I'll add the vehicle in the world(MUTEX)
    pthread_mutex_lock(&(mutex));
    Vehicle* new_vehicle = malloc(sizeof(Vehicle));
    Vehicle_init(new_vehicle, &world, id, received_texture->image);
    World_addVehicle(&world, new_vehicle);

    // I'll add the user in my personal user list(GLOBAL)
    User_insert_last(users, user);
    pthread_mutex_unlock(&(mutex));
    // I wanna notify the users connected that a new vehicle has been added
    // I will also send its texture, so that everybody can load its texture
    PacketHeader header_connect;
    header_connect.type = NewConnection;

    ImagePacket* user_connected = (ImagePacket*) malloc(sizeof(ImagePacket));
    user_connected->header = header_connect;
   	user_connected->id = user->id;
   	user_connected->image = received_texture->image;
    
    User* user_aux = users->first;

    int msg_length = 0;
    char buffer_connection[BUFFER_SIZE];
    int packet_connection = Packet_serialize(buffer_connection, &(user_connected->header)); // I'll get the written bytes

    // I'll send the connection packet to the other connected users so that everybody can load the new texture
    while (user_aux != NULL) {
    	if (user_aux->id != user->id) {
		    msg_length = 0;
		    while(msg_length < packet_connection){
		      ret = send(user_aux->id, buffer_connection + msg_length, packet_connection - msg_length,0);
		      if (ret==-1 && 
              errno==EINTR) continue;
		      ERROR_HELPER(ret, "[ERROR] Error sending new user to other users!!!");
		      if (ret==0) break;
		      msg_length += ret;
		    }
	    }

	    user_aux = user_aux->next;
  	}

  	// the new arrival vehicle has to know the other existing vehicle in the world
  	user_aux = users->first;

  	while (user_aux != NULL) {
    	if (user_aux->id != user->id) {
    		// Invia al nuovo utente la connessione degli utenti già online
    		char buffer_connection_new[BUFFER_SIZE];
		    
		    PacketHeader header_new;
		    header_new.type = NewConnection;

		    ImagePacket* existing_vehicle = (ImagePacket*) malloc(sizeof(ImagePacket));
		    existing_vehicle->header = header_new;
		   	existing_vehicle->id = user_aux->id;
		   	existing_vehicle->image = World_getVehicle(&world, user_aux->id)->texture;

		    packet_connection = Packet_serialize(buffer_connection_new, &(existing_vehicle->header));

		    // Invio del pacchetto serializzato
		    msg_length = 0;
		    while(msg_length < packet_connection){
		      ret = send(user->id, buffer_connection_new + msg_length, packet_connection - msg_length,0);
		      if (ret==-1 && errno==EINTR) continue;
		      ERROR_HELPER(ret, "[ERROR] Error sending online users to the new user!!!");
		      if (ret==0) break;
		      msg_length += ret;
		    }

		    // Attende la conferma del client
			while( (ret = recv(user->id, buffer_connection_new, 12, 0)) < 0){
				if (ret==-1 && errno == EINTR) continue;
				ERROR_HELPER(ret, "[ERROR] Failed to receive packet!!!");
			}

	    }

	    user_aux = user_aux->next;
  	}

    return 1;
  }

  // I don't know the packet the client sent me
  else {
    printf("[ERROR] Unknown packet received from %d!!!\n", id);   // DEBUG OUTPUT
  }

  return -1;  // Return 
}

// Handling client thread for adding user in the UserList 
// and packets checking through TCP_packet

void* TCP_client_thread (void* args){
  tcp_args_t* tcp_args = (tcp_args_t*) args;

  printf("%sHandling client %d\n",TCP, tcp_args->client_desc);

  int client_tcp_socket = tcp_args->client_desc;
  int msg_length = 0;
  int ret;
  char buffer_recv[BUFFER_SIZE];

  // I'll prepare the new user to be added in the list
  User* user = (User*) malloc(sizeof(User));
  user->id = client_tcp_socket;
  user->user_addr_tcp = tcp_args->client_addr;
  user->x = 0;
  user->y = 0;
  user->theta = 0;
  user->translational_force = 0;
  user->rotational_force = 0;

  // Receiving packet, int this packet may be things like the id request or the texture request. Following the handling
  int packet_length = BUFFER_SIZE;
  while(running) {
    while( (ret = recv(client_tcp_socket, buffer_recv + msg_length, packet_length - msg_length, 0)) < 0){
    	if (ret==-1 && errno == EINTR) continue;
    	ERROR_HELPER(ret, "[ERROR] Failed to receive packet!!!");
        usleep(200);
    }
    // The user disconnected
    if (ret == 0) {
      printf("%s*************** Client Disconnected, ID: %d ***************\n", TCP,user->id);
      
      //Detaching (MUTEX)
      
      pthread_mutex_lock(&(mutex));
      //Critic section
      User_detach(users, user->id);
      Vehicle* deleted = World_getVehicle(&world, user->id);
      Vehicle* aux = World_detachVehicle(&world, deleted);
      Vehicle_destroy(aux);
      
      pthread_mutex_unlock(&(mutex));

      // I wanna notify the other users that one guy left, I'll add the id of the gone user
      PacketHeader header_aux;
      header_aux.type = NewDisconnection;

      IdPacket* user_disconnected = (IdPacket*) malloc(sizeof(IdPacket));
      user_disconnected->header = header_aux;
      user_disconnected->id = client_tcp_socket;

      User* user_aux = users->first;

      while (user_aux != NULL) {
        char buffer_disconnection[BUFFER_SIZE];
      	int packet_disconnection = Packet_serialize(buffer_disconnection, &(user_disconnected->header));

	    // Sending
	    msg_length = 0;
	    while(msg_length < packet_disconnection){
	      ret = send(user_aux->id, buffer_disconnection + msg_length, packet_disconnection - msg_length,0);
	      if (ret==-1 && errno==EINTR) continue;
	      ERROR_HELPER(ret, "[ERROR] Error sending user disconnection to other clients!!!");
	      if (ret==0) break;
	      msg_length += ret;
	    }

        user_aux = user_aux->next;
      }

      break;
    }
    //if I got in here it means that I received correctly a packet, no disconnections or else
    msg_length += ret;

    // what kind of packet have I received?
    ret = TCP_packet(client_tcp_socket, tcp_args->client_desc, buffer_recv, tcp_args->surface_elevation, tcp_args->elevation_texture, msg_length, user);

    if (ret == 1) {
      msg_length = 0;
      continue;
    }
    else continue;
  }

  // Chiusura thread
  pthread_exit(0);
}


// Handler TCP connection with the client (thread-part)
//This function allows me to accept more than one connection so that the application becomes multithreading
void* TCP_handler(void* args){
  int ret;
  int tcp_client_desc;

  tcp_args_t* tcp_args = (tcp_args_t*) args;

  printf("%s Waiting for connections from clients\n",TCP);

  int sockaddr_len = sizeof(struct sockaddr_in);
  struct sockaddr_in client_addr;

  while( (tcp_client_desc = accept(tcp_socket, (struct sockaddr*)&client_addr, (socklen_t*) &sockaddr_len)) > 0) {
    printf( " *************** New Client Connected ***************\n"); 
    printf("%sConnection enstablished with %d...\n",TCP, tcp_client_desc);

    pthread_t client_thread;

    // args client thread
    tcp_args_t tcp_args_aux;
    tcp_args_aux.client_desc = tcp_client_desc;
    tcp_args_aux.elevation_texture = tcp_args->elevation_texture;
    tcp_args_aux.surface_elevation = tcp_args->surface_elevation;
    tcp_args_aux.client_addr = client_addr;

    // I'm creating a new thread for each client connecting
    ret = pthread_create(&client_thread, NULL, TCP_client_thread, &tcp_args_aux);
    PTHREAD_ERROR_HELPER(ret, "[ERROR] Failed to create TCP client handling thread!!!");
  }
  ERROR_HELPER(tcp_client_desc, "[ERROR] Failed to accept client TCP connection!!!");

  printf("%sStopped accepting connection\n",TCP);

  // Chiusura thread
  pthread_exit(0);
}


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~UDP PART~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void udp_initialization(void){
  int ret;
  udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  ERROR_HELPER(udp_socket, "[ERROR] Failed to create UDP socket!!!");

  struct sockaddr_in udp_server_addr = {0};
  udp_server_addr.sin_addr.s_addr = INADDR_ANY;
  udp_server_addr.sin_family      = AF_INET;
  udp_server_addr.sin_port        = htons(UDP_PORT);

  int reuseaddr_opt_udp = 1;
  ret = setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_opt_udp, sizeof(reuseaddr_opt_udp));
  ERROR_HELPER(ret, "[ERROR] Failed setsockopt on UDP server socket!!!");

  ret = bind(udp_socket, (struct sockaddr*) &udp_server_addr, sizeof(udp_server_addr));
  ERROR_HELPER(ret, "[ERROR] Failed bind address on UDP server socket!!!");

  printf("%sServer UDP started\n",UDP);
}

// Handler UDP connection with client, this function is the receiver
void* UDP_receiver(void* args) {
  int ret;
  char buffer_recv[BUFFER_SIZE];
  
  struct sockaddr_in client_addr = {0};
  socklen_t addrlen = sizeof(struct sockaddr_in);
  
  printf("%sReady to receive updates\n",UDP);
  while (1) {
    if((ret = recvfrom(udp_socket, buffer_recv, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &addrlen)) > 0) {}
    ERROR_HELPER(ret, "[ERROR] Error receiving UDP packet!!!");

    // Getting the received packet
    PacketHeader* header = (PacketHeader*) buffer_recv;

    VehicleUpdatePacket* packet = (VehicleUpdatePacket*)Packet_deserialize(buffer_recv, header->size);
    User* user = User_find_id(users, packet->id);
    user->user_addr_udp = client_addr;

    if(!user) {
      printf("Cant find the user: %d\n", packet->id);
      pthread_exit(0);
    }
    
    // Update the user positions in the world
    Vehicle* vehicle_aux = World_getVehicle(&world, user->id);

    vehicle_aux->translational_force_update = packet->translational_force;
    vehicle_aux->rotational_force_update = packet->rotational_force;

    // Situation update, since every client sends this, I've got to update the world in mutual exclusion
    pthread_mutex_lock(&(mutex));
    World_update(&world);
    pthread_mutex_unlock(&(mutex));
  }

  pthread_exit(0);
}

// Handler UDP connection with client, this function is the sender
void* UDP_sender(void* args) {
  char buffer_send[BUFFER_SIZE];

  printf("%sReady to send updates\n",UDP);
  while(running) {
    int size = users->size;

    // If one user is, at least, connected
    if (size > 0) {
        
      // I'll prepare the world update to notify the changes in the world
      PacketHeader header;
      header.type = WorldUpdate;

      WorldUpdatePacket* world_update = (WorldUpdatePacket*) malloc(sizeof(WorldUpdatePacket));
      world_update->header = header;
      world_update->updates = (ClientUpdate*) malloc(sizeof(ClientUpdate) * size);
      world_update->num_vehicles = users->size;
      
      // I don't want race conditions on this
      pthread_mutex_lock(&(mutex));
      User* user = users->first;

      for (int i=0; i<size; i++) {
      	// Update creation for the i-esim user
        ClientUpdate* client = &(world_update->updates[i]);

        Vehicle* user_vehicle = World_getVehicle(&world, user->id);
        client->id = user->id;
        client->x = user_vehicle->x;
        client->y = user_vehicle->y;
        client->theta = user_vehicle->theta;

        user = user->next;
      }

      // Update packet serialization
      int size = Packet_serialize(buffer_send, &world_update->header);

      pthread_mutex_unlock(&(mutex));
      user = users->first;

      // Every connected user must receive this
      while (user != NULL) {
        if(user->user_addr_udp.sin_addr.s_addr != 0) {
          int ret = sendto(udp_socket, buffer_send, size, 0, (struct sockaddr*) &user->user_addr_udp, (socklen_t)sizeof(user->user_addr_udp));
          ERROR_HELPER(ret, "Error in sending updates");
        }
        user = user->next;
      }
    }

    // I'll wait some time before bothering every other user ;)
    usleep(GOOD_NIGHT);
  }

  pthread_exit(0);
}


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~MAIN~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
int main(int argc, char **argv) {
  running = 0;

  if (argc<3) {
    printf("usage: %s <elevation_image> <texture_image>\n", argv[1]);
    exit(-1);
  }
  printf("\n");
  printf(" ***********Hello I'm a server, I'm waiting here to make you play ;)**************** \n");

  int ret;

  // Inizializzazione del signal handler
  struct sigaction signal_action;
  signal_action.sa_handler = signalHandler;
  signal_action.sa_flags = SA_RESTART;

  sigfillset(&signal_action.sa_mask);
  ret = sigaction(SIGHUP, &signal_action, NULL);
  ERROR_HELPER(ret,"[ERROR] Cannot handle SIGHUP!!!");
  ret = sigaction(SIGINT, &signal_action, NULL);
  ERROR_HELPER(ret,"[ERROR] Cannot handle SIGINT!!!");
  ret = sigaction(SIGSEGV, &signal_action, NULL);
  ERROR_HELPER(ret, "[ERROR] Cannot handle SIGSEGV!!!");
  ret = sigaction(SIGTERM, &signal_action, NULL);
  ERROR_HELPER(ret,"[ERROR] Cannot handle SIGTERM!!!");

  char* elevation_filename=argv[1];
  char* texture_filename=argv[2];
  
  // load the images
  surface_elevation = Image_load(elevation_filename);
  if (surface_elevation) {
    //printf("Done! \n");
  } else {
    //printf("Fail! \n");
  }

  //printf("loading texture image from %s ... ", texture_filename);
  surface_texture = Image_load(texture_filename);
  if (surface_texture) {
    //printf("Done! \n");
  } else {
    //printf("Fail! \n");
  }

  // TCP stuff
  tcp_initialization();

  // UDP STUFF
  
  udp_initialization();

  // Creating a new user list
  users = (UserHead*) malloc(sizeof(UserHead));
  Users_init(users);

  // Let's set up the world
  World_init(&world, surface_elevation, surface_texture,  0.5, 0.5, 0.5);

  /* THREAD PART */

  // Thread TCP args
  tcp_args_t tcp_args;
  tcp_args.elevation_texture = surface_texture;
  tcp_args.surface_elevation = surface_elevation;

  // I'm running
  running = 1;

  // Thread create 
  ret = pthread_create(&TCP_connection, NULL, TCP_handler, &tcp_args);
  PTHREAD_ERROR_HELPER(ret, "[ERROR] Failed to create TCP connection thread!!!");

  ret = pthread_create(&UDP_sender_thread, NULL, UDP_sender, NULL);
  PTHREAD_ERROR_HELPER(ret, "[ERROR] Failed to create UDP sender thread!!!");

  ret = pthread_create(&UDP_receiver_thread, NULL, UDP_receiver, NULL); 
  PTHREAD_ERROR_HELPER(ret, "[ERROR] Failed to create UDP receiver thread!!!");

  // Thread join 
  ret = pthread_join(TCP_connection,NULL);
  ERROR_HELPER(ret,"[ERROR] Failed to join TCP server connection thread!!!");

  ret = pthread_join(UDP_sender_thread,NULL);
  ERROR_HELPER(ret,"[ERROR] Failed to join UDP server sender thread!!!");

  ret = pthread_join(UDP_receiver_thread,NULL);
  ERROR_HELPER(ret,"[ERROR] Failed to join UDP server receiver thread!!!");

  return 0;     
}
