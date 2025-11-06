#ifndef HASH_DELEE_H_
#define HASH_DELEE_H_

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "rdma_server.h"

// #define HT_SZ 1024
// #define HT_SEED 4426

typedef struct HashNode {
	int key;		//sockfd
	struct client_r_info *value;
	unsigned int index;
	struct HashNode *next_node;
} HashNode;

typedef struct SocketNode {
	int key;		//sockfd
	struct socket_info *value;
	unsigned int index;
	struct SocketNode *next_node;
} SocketNode;

typedef struct HashTable {
        HashNode* table[HT_SZ];
        int size;
		pthread_rwlock_t lock;   // readers-writer lock
} HashTable;

typedef struct SocketHashTable {
        SocketNode* table[HT_SZ];
        int size;
		pthread_rwlock_t lock;   // readers-writer lock
} SocketHashTable;

unsigned int MurmurHash3(const void *key, int len, unsigned int seed);
unsigned int hashFunction(int key);
HashTable* createHashTable();
bool insertByValue(HashTable* hashTable, int key, struct client_r_info *value);
struct client_r_info * searchByKey(HashTable* hashTable, int key);
int searchByValue(HashTable* hashTable, struct client_r_info *value);
void deleteByKey(HashTable* hashTable, int key);
void deleteByValue(HashTable* hashTable, struct client_r_info *value);
void freeHashTable(HashTable* hashTable);

/* for SocketNode */
SocketHashTable* createSocketHashTable();
bool socket_insertByValue(SocketHashTable* hashTable, int key, struct socket_info *value);
struct socket_info* socket_searchByKey(SocketHashTable* hashTable, int key);
int socket_searchByValue(SocketHashTable* hashTable, struct socket_info *value);
void socket_deleteByKey(SocketHashTable* hashTable, int key);
void socket_deleteByValue(SocketHashTable* hashTable, struct socket_info *value);
void socket_freeHashTable(SocketHashTable* hashTable);

#endif
