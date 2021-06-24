#include <GL/glut.h>
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
#include "semaphore.h"

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~GLOBALS~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

#define TIME_TO_SLEEP    40000
#define WORLD_SIZE       10

typedef struct {
    struct sockaddr_in server_addr;
    int id;
} udp_args_t;

typedef struct {
    struct sockaddr_in server_addr;
    int id;
} tcp_args_t;

typedef struct {
  volatile int run;
  World* world;
} UpdaterArgs;

int window;
WorldViewer viewer;
World world;
Vehicle* vehicle;

pthread_mutex_t mutex;

char* server_address = SERVER_ADDRESS;

int running;
int udp_socket, tcp_socket;
pthread_t TCP_connection, UDP_sender, UDP_receiver, runner_thread;

int my_id;
Image* map_elevation;
Image* map_texture;
Image* my_texture_from_server;

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~AUXILIARS FUNCTIONS~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

// Once my code terminates I want to free the memory
void freeMemory(void) {
  int ret;
    printf("Free Memory: Preparing to deallocate everything I used\n");
  running = 0;
  ret = close(tcp_socket);
  ERROR_HELPER(ret, "Cannot close TCP socket");
  ret = close(udp_socket);
  ERROR_HELPER(ret, "Cannot close UDP socket");
  ret = pthread_cancel(UDP_sender);
  ERROR_HELPER(ret, "Cannot cancel the UDP sender thread");
  ret = pthread_cancel(UDP_receiver);
  ERROR_HELPER(ret, "Cannot cancel the UDP receiver thread");
  ret = pthread_cancel(runner_thread);
  ERROR_HELPER(ret, "Cannot cancel the world runner thread");
  ret = pthread_cancel(TCP_connection);
  ERROR_HELPER(ret, "Cannot cancel the TCP connection thread");

  World_destroy(&world);
  Image_free(map_elevation);
  Image_free(map_texture);
  Image_free(my_texture_from_server);

  printf("Free Memory: Memory cleaned...\n");
  return;
}

// Handling signals function
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
	default:
	  printf("Uncaught signal: %d\n", signal);
	  return;
  }
  freeMemory();
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~UDP PART~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

// Here I provide sending my actual position through udp protocol
void* UDP_Sender(void* args){
  int ret;
  char buffer[BUFFER_SIZE];

  printf("%sSender thread started\n",UDP);

  // Setting up connection
  udp_args_t* udp_args = (udp_args_t*) args;
  int id = udp_args->id;

  //Needed to sendto
  struct sockaddr_in server_addr = udp_args->server_addr;
  int sockaddr_len = sizeof(struct sockaddr_in);

  while(running) {
    // This is my update to the server
    VehicleUpdatePacket* vehicle_packet = (VehicleUpdatePacket*) malloc(sizeof(VehicleUpdatePacket));

    PacketHeader header;
    header.type = VehicleUpdate;
    vehicle_packet->header = header;
    vehicle_packet->id = id;

    vehicle_packet->rotational_force = vehicle->rotational_force_update;
    vehicle_packet->translational_force = vehicle->translational_force_update;
    
    // Printing my actual coordinates in the world
    /*printf("X: %.2f, Y: %.2f, THETA: %.2f, ROTATIONAL: %.2f, TRANSLATIONAL: %.2f\n", vehicle->x,vehicle->y,vehicle->theta,vehicle->rotational_force_update,
     * vehicle->translational_force_update);*/

    // Serialization
    int buffer_size = Packet_serialize(buffer, &vehicle_packet->header);

    // Sending my packet
    ret = sendto(udp_socket, buffer, buffer_size , 0, (struct sockaddr*) &server_addr, (socklen_t) sockaddr_len);
    ERROR_HELPER(ret,"Failed sending updates");

    usleep(TIME_TO_SLEEP);
  }

  pthread_exit(0);
}

/* Receiving world updates and loading them in my world */
void* UDP_Receiver(void* args){
  int ret;
  int buffer_size = 0;
  char buffer[BUFFER_SIZE];

  printf("%sReceiving updates\n",UDP);

  // Setting up a connection
  udp_args_t* udp_args = (udp_args_t*) args;

  //Needed to recvfrom
  struct sockaddr_in server_addr = udp_args->server_addr;
  socklen_t addrlen = sizeof(struct sockaddr_in);

  // Receiving updates
  while ( (ret = recvfrom(udp_socket, buffer, BUFFER_SIZE, 0, (struct sockaddr*) &server_addr, &addrlen)) > 0){   
  
    buffer_size += ret;

    // Deserialization
    WorldUpdatePacket* world_update = (WorldUpdatePacket*) Packet_deserialize(buffer, buffer_size);

    // I'll update all the vehicle's movement in the map
    for(int i=0; i < world_update->num_vehicles; i++) {
      ClientUpdate* client = &(world_update->updates[i]);

      Vehicle* client_vehicle = World_getVehicle(&world, client->id);

      if (client_vehicle == 0) continue;

      client_vehicle = World_getVehicle(&world, client->id);
      client_vehicle->x = client->x;
      client_vehicle->y = client->y;
      client_vehicle->theta = client->theta;
    }
  }
  printf("The server died\n");
  pthread_exit(0);
}

// this is the updater threas that takes care of refreshing the agent position
// in your client it is not needed, but you will
// have to copy the x,y,theta fields from the world update packet
// to the vehicles having the corrsponding id
void* updater_thread(void* args_){
  UpdaterArgs* args=(UpdaterArgs*)args_;
  while(args->run){
    usleep(TIME_TO_SLEEP);
  }
  return 0;
}
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~TCP PART~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void tcp_initialization(void){
    
    int ret;
    struct sockaddr_in server_addr = {0};

    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_HELPER(tcp_socket, "Could not create socket\n");

    server_addr.sin_addr.s_addr = inet_addr(server_address);
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(TCP_PORT);

    ret = connect(tcp_socket, (struct sockaddr*) &server_addr, sizeof(struct sockaddr_in)); 
    ERROR_HELPER(ret, "Unable to create connection\n"); 
    printf("%sConnection enstablished with server\n\n",TCP);
    return;
}
 
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~GETTING AN ID~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void recv_ID(int* my_id) {
  int ret;
  char BUFFER[BUFFER_SIZE];

  printf("%sRequesting ID\n",TCP);
  
  // Setting the packet 
  PacketHeader header;
  header.type = GetId;
  
  IdPacket* packet = (IdPacket*)malloc(sizeof(IdPacket));
  packet->header = header;
  packet->id = -1;

  int size = Packet_serialize(BUFFER, &packet->header);

  // I'll send the request via TCP
  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0 ) {
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "[ERROR] Cannot write to socket sending id request!!!");
  }

  // The server got my request and now let's see what Id I got
  while ( (size = recv(tcp_socket, BUFFER, BUFFER_SIZE, 0)) < 0 ) { //ricevo ID dal server
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "[ERROR] Cannot read from socket receiving assigned id!!!");
  }
  printf("%sId successfully received\n",TCP);
  // Extracting the id and saving it
  IdPacket* id_recv = (IdPacket*) Packet_deserialize(BUFFER,size);
  *my_id = id_recv->id;

  printf("%sI am the Client: %d\n", TCP,*my_id);
  printf("\n");
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~GETTING TEXTURE MAP~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
void recv_Texture(Image** map_texture) {
  int ret;
  char BUFFER[BUFFER_SIZE];

  printf("%sRequesting map texture\n",TCP);

  PacketHeader header;
  header.type = GetTexture;
  ImagePacket* packet = (ImagePacket*) malloc(sizeof(ImagePacket));
  packet->image = NULL;
  packet->header = header;

  int size = Packet_serialize(BUFFER, &packet->header);

  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0 ) {
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "[ERROR] Cannot write to socket sending map texture request!!!");
  }

  int whole_packet_size = 0;
  size = 0;

  while(1) {
    while ( (size += recv(tcp_socket, BUFFER + size, BUFFER_SIZE - size, 0)) < 0 ) {
      if (errno == EINTR) continue;
      ERROR_HELPER(-1, "[ERROR] Cannot read from socket receiving map texture!!!");
    }

    // This is the whole packet size
    PacketHeader* aux = (PacketHeader*) BUFFER;
    whole_packet_size = aux->size;
    
    // If the packet's dimension is less than the whole size packet I got to continue receiving
    if (size < whole_packet_size) continue;
    else break;
  }

  // Load della texture della mappa
  ImagePacket* received_packet = (ImagePacket*) Packet_deserialize(BUFFER,size);
  *map_texture = received_packet->image;

  printf("%sMap texture received\n\n",TCP);
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~GETTING ELEVATION MAP~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
void recv_Elevation(Image** map_elevation) {
  int ret;
  char BUFFER[BUFFER_SIZE];

  printf("%sRequesting map elevation\n",TCP);

  // Preparazione del pacchetto da inviare
  PacketHeader header;
  header.type = GetElevation;
  ImagePacket* packet = (ImagePacket*) malloc(sizeof(ImagePacket));
  packet->image = NULL;
  packet->header = header;

  // Serializza il pacchetto
  int size = Packet_serialize(BUFFER, &packet->header);

  // Invia la richiesta
  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0 ) {
    if (errno == EINTR) continue;
    ERROR_HELPER(-1, "[ERROR] Cannot write to socket sending map elevation request!!!");
  }

  int whole_packet_size = 0;
  size = 0;
  while(1) {
    // Riceve la elevation della mappa
    while ( (size += recv(tcp_socket, BUFFER + size, BUFFER_SIZE - size, 0)) < 0 ) {
      if (errno == EINTR) continue;
      ERROR_HELPER(-1, "[ERROR] Cannot read from socket receiving map elevation!!!");
    }

    // Dimensione totale del pacchetto da ricevere
    PacketHeader* aux = (PacketHeader*) BUFFER;
    whole_packet_size = aux->size;

    // Se la dimensione del pacchetto ricevuto è ancora minore della dimensione del pacchetto totale aspetta le altre parti
    if (size < whole_packet_size) 
        continue;
    else 
        break;
  }

  // Load della elevation della mappa
  ImagePacket* elevation_packet = (ImagePacket*) Packet_deserialize(BUFFER, size);
  *map_elevation = elevation_packet->image;

  printf("%sMap elevation received\n\n",TCP);
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~SENDING MY TEXTURE TO SERVER~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
void send_Texture(Image** my_texture, Image** my_texture_from_server) {
  int ret;
  char BUFFER[BUFFER_SIZE];

  printf("%sSending my texture to server\n",TCP);

  // Preparazione del pacchetto da inviare
  PacketHeader header;
  header.type = PostTexture;

  ImagePacket* packet = (ImagePacket*)malloc(sizeof(ImagePacket));
  packet->image = *my_texture;
  packet->header = header;

  int size = Packet_serialize(BUFFER, &packet->header);
  
  // Invia la texture del veicolo
  while ( (ret = send(tcp_socket, BUFFER, size, 0)) < 0) {
    if (errno == EINTR) continue;
    ERROR_HELPER(ret, "[ERROR] Cannot write to socket sending vehicle texture!!!");
  }

  printf("%sVehicle texture sent\n\n",TCP);

  // I now upload my texture so that I can load my texture on the vehicle lately
  *my_texture_from_server = packet->image;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~RECEIVES UPDATES ON THE STATE FROM THE SERVER~~~~~~~~~~~~~~*/
void* TCP_thread_routine(void* args) {

	printf("%sTCP_THREAD STARTED\n",TCP);

	while(running) {
		char BUFFER[BUFFER_SIZE];
		int actual_size = 0;
		int size = 0;
        int ret=0;

		while(1) {
            while ( (size += recv(tcp_socket, BUFFER + size, BUFFER_SIZE - size, 0)) < 0 ) {
                if (errno == EINTR) continue;
                ERROR_HELPER(-1, "[ERROR] Cannot read from socket on TCP ROUTINE!!!");
            }
            if(size==0){
                printf("\n***Server accidentally stopped for some reason***\n");
                freeMemory();
            }
            
            PacketHeader* aux = (PacketHeader*) BUFFER;
            actual_size = aux->size;
          
            // I didn't receive the entire message
            if (size < actual_size) continue;

            PacketHeader* head = (PacketHeader*) Packet_deserialize(BUFFER, actual_size);
            // The server has successfully sent me the status of the application, let's see what's changed
              
            if(head->type == NewConnection) {
            /* If I'm here means that the server has sent me a new texture, meaning that a new client has connected */
                ImagePacket* texture_back = (ImagePacket*) Packet_deserialize(BUFFER, actual_size);
                Image* new_texture_user = texture_back->image;
                pthread_mutex_lock(&(mutex));
                Vehicle* v = (Vehicle*) malloc(sizeof(Vehicle));
                Vehicle_init(v, &world, texture_back->id, new_texture_user);
                World_addVehicle(&world, v);
                pthread_mutex_unlock(&(mutex));
                printf("%sUser %d has just joined the game...\n",TCP, texture_back->id);

                // The client provide to notify he has registrated a new user
                
                PacketHeader* pack = (PacketHeader*)malloc(sizeof(PacketHeader));
                pack->type = NewClient;

                actual_size = Packet_serialize(BUFFER, pack);

                while ( (ret = send(tcp_socket, BUFFER,actual_size , 0)) < 0) {
                  if (errno == EINTR) continue;
                  ERROR_HELPER(ret, "[ERROR] Cannot write to socket client is becoming ready!!!\n");
                }
               // I need to exit the loop, so that I can continue receiving packet from the server
               // That's possible thanks to the while(running) loop outside this one
                break;
            }

              // Se si disconnette un user
            else if(head->type == NewDisconnection) {
                IdPacket* id_disconnected = (IdPacket*) Packet_deserialize(BUFFER, size);
                /* I got to destroy a vehicle*/
                Vehicle* deleting_user = World_getVehicle(&world, id_disconnected->id);
                if(deleting_user) { //checking if it's in my world
                    pthread_mutex_lock(&(mutex));
                    World_detachVehicle(&world, deleting_user);
                    Vehicle_destroy(deleting_user);
                    pthread_mutex_unlock(&(mutex));
                }

                printf("%sUser %d disconnected\n",TCP, id_disconnected->id);
                // I need to exit the loop, so that I can continue receiving packet from the server
               // That's possible thanks to the while(running) loop outside this one
                break;
            }
            
            else if(head->type == ServerOut) {
                printf("Server died unexpectly\n");
                freeMemory();
                return NULL;
            }
            
            /* We will never get in here*/
            else {
                printf("%sERROR, I didn't manage to recognize this packet: %d...\n",TCP, head->type);
                continue;
              }
              size=0;
		}
	}
    //When running will be set to zero
	printf("%s Tcp routine stopped\n",TCP);

  pthread_exit(0);
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~PRELIMINARS INFO THROUGH TCP PROTOCOL~~~~~~~~~~~~~~~~~~~~~~~*/
void helloServer (int* my_id, Image** my_texture, Image** map_elevation,Image** map_texture, Image** my_texture_from_server){
  recv_ID(my_id);
  recv_Texture(map_texture);
  recv_Elevation(map_elevation);
  send_Texture(my_texture, my_texture_from_server);

  return;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~MAIN~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
int main(int argc, char **argv) {
    printf("\n");
    printf("*****************Hi I am a Client! I will now connect to the main server******************\n");
    printf("To move yourself in the world you can use either the arrow keys or the controller if you have on ;)\n");
    printf("To zoom out by your controller just press square, to zoom in press the X button\n");
    printf("To change the view you can press either options, triangle or circle\n");
    printf("To quit the game press R1\n");
    printf("Enjoy!\n");
    
  running = 0;

  if (argc<2) {
    printf("usage: %s <player texture>\n", argv[1]);
    exit(-1);
  }
  int ret;

  // Inizializzazione del signal handler
  struct sigaction signal_action;
  signal_action.sa_handler = signalHandler;
  signal_action.sa_flags = SA_RESTART;

  sigfillset(&signal_action.sa_mask);
  ret = sigaction(SIGHUP, &signal_action, NULL);
  ERROR_HELPER(ret,"Cannot handle SIGHUP!!!");
  ret = sigaction(SIGINT, &signal_action, NULL);
  ERROR_HELPER(ret,"Cannot handle SIGINT!!!");
  ret = sigaction(SIGTERM, &signal_action, NULL);
  ERROR_HELPER(ret,"Cannot handle SIGTERM!!!");

  //printf("loading texture image from %s ... ", argv[1]);
  Image* my_texture = Image_load(argv[1]);
  if (my_texture) {
    //printf("Done! \n");
  } else {
    //printf("Fail! \n");
  }
  
  //Connecting locally
  server_address = "127.0.0.1";
  
  //Image* my_texture_for_server;
  // todo: connect to the server
  //   -get ad id
  //   -send your texture to the server (so that all can see you)
  //   -get an elevation map
  //   -get the texture of the surface

  // Apertura connessione TCP
  
  tcp_initialization();

  // Apertura connessione UDP
   struct sockaddr_in udp_server = {0}; // some fields are required to be filled with 0

  udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  ERROR_HELPER(udp_socket, "Can't create an UDP socket!!!");

  udp_server.sin_addr.s_addr = inet_addr(server_address);
  udp_server.sin_family      = AF_INET;
  udp_server.sin_port        = htons(UDP_PORT);


  running = 1;

  // Richiede l'ID, la texture e l'elevation della mappa e invia la propria texture al server che la rimanda indietro
  helloServer(&my_id, &my_texture, &map_elevation, &map_texture, &my_texture_from_server);

  // Carica il mondo con le texture e elevation ricevute
  World_init(&world, map_elevation, map_texture, 0.5, 0.5, 0.5);

  // Aggiunge il nostro veicolo al mondo locale
  vehicle=(Vehicle*) malloc(sizeof(Vehicle));
  Vehicle_init(vehicle, &world, my_id, my_texture_from_server);
  World_addVehicle(&world, vehicle);
  
  // spawn a thread that will listen the update messages from
  // the server, and sends back the controls
  // the update for yourself are written in the desired_*_force
  // fields of the vehicle variable
  // when the server notifies a new player has joined the game
  // request the texture and add the player to the pool
  /*FILLME*/
    
  //This struct contains info like the client's id or the whole udp info packet
  udp_args_t udp_args;
  udp_args.server_addr = udp_server;
  udp_args.id = my_id;

  pthread_attr_t runner_attrs;
  UpdaterArgs runner_args={
    .run=1,
    .world=&world
  };
  
  pthread_attr_init(&runner_attrs);
  runner_args.run=0;
  void* retval;
  
  ret = pthread_create(&TCP_connection, NULL, TCP_thread_routine, NULL);
  PTHREAD_ERROR_HELPER(ret, "Can't create TCP connection receiver thread");

  ret = pthread_create(&UDP_sender, NULL, UDP_Sender, &udp_args);
  PTHREAD_ERROR_HELPER(ret, "Can't create UDP Sender thread");

  ret = pthread_create(&UDP_receiver, NULL, UDP_Receiver, &udp_args);
  PTHREAD_ERROR_HELPER(ret, "Can't create UDP receiver thread");

  ret = pthread_create(&runner_thread, &runner_attrs, updater_thread, &runner_args);
  PTHREAD_ERROR_HELPER(ret, "Can't create runner thread");

  // Apre la schermata di gioco
  WorldViewer_runGlobal(&world, vehicle, &argc, argv);

  ret = pthread_join(TCP_connection, NULL);
  PTHREAD_ERROR_HELPER(ret, "Cannot join TCP connection thread");

  ret = pthread_join(UDP_sender, NULL);
  PTHREAD_ERROR_HELPER(ret, "Cannot join UDP sender thread");

  ret = pthread_join(UDP_receiver, NULL);
  PTHREAD_ERROR_HELPER(ret, "Cannot join UDP receiver thread");

  ret = pthread_join(runner_thread, &retval);
  PTHREAD_ERROR_HELPER(ret, "Cannot run world\n");

  freeMemory();

  return 0;  
}
