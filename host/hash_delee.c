#include "hash_delee.h"

unsigned int MurmurHash3(const void *key, int len, unsigned int seed)
{
	const uint8_t *data = (const uint8_t *)key;
	const int nblocks = len / 4;
	unsigned int h1 = seed;
	const unsigned int c1 = 0xcc9e2d51;
	const unsigned int c2 = 0x1b873593;

	// body
	const unsigned int *blocks = (const unsigned int *)(data + nblocks * 4);
	for (int i = -nblocks; i; i++) {
		unsigned int k1 = blocks[i];
		k1 *= c1;
		k1 = (k1 << 15) | (k1 >> (32 - 15));
		k1 *= c2;

		h1 ^= k1;
		h1 = (h1 << 13) | (h1 >> (32 - 13));
		h1 = h1 * 5 + 0xe6546b64;
	}

	// tail
	const uint8_t *tail = (const uint8_t *)(data + nblocks * 4);
	unsigned int k1 = 0;
	switch (len & 3) {
		case 3: k1 ^= tail[2] << 16;
		case 2: k1 ^= tail[1] << 8;
		case 1: k1 ^= tail[0];
			k1 *= c1;
			k1 = (k1 << 15) | (k1 >> (32 - 15));
			k1 *= c2;
			h1 ^= k1;
	}

	// finalization
	h1 ^= len;
	h1 ^= h1 >> 16;
	h1 *= 0x85ebca6b;
	h1 ^= h1 >> 13;
	h1 *= 0xc2b2ae35;
	h1 ^= h1 >> 16;

	return h1;
}

// 해시 함수 (키를 사용하여 해시 인덱스 생성)
unsigned int hashFunction(int key) {
	unsigned int index = MurmurHash3(&key, sizeof(int), HT_SEED);

	return index % HT_SZ;  // 간단한 모듈로 연산으로 인덱스 계산
}

// 해시 테이블 생성 함수
HashTable* createHashTable() {
	HashTable* newTable = (HashTable*)malloc(sizeof(HashTable));
	newTable->size = HT_SZ;

	// 테이블을 NULL로 초기화
	for (int i = 0; i < HT_SZ ; i++) {
		newTable->table[i] = NULL;
	}

	// pthread_rwlock_init(&newTable->lock, NULL);
	return newTable;
}

// 키와 값을 입력받아 해시 테이블에 삽입하는 함수
bool insertByValue(HashTable* hashTable, int key, struct client_r_info *value)
{
	// pthread_rwlock_wrlock(&hashTable->lock);

	unsigned int index = hashFunction(key);

	// 새로운 노드 생성
	HashNode* newNode = (HashNode*)malloc(sizeof(HashNode));
	if (newNode == NULL) {
		// pthread_rwlock_unlock(&hashTable->lock);
		return false; // 메모리 할당 실패
	}
	newNode->key = key;
	newNode->value = value;
	newNode->index = index;
	newNode->next_node = NULL; // 초기화


	// 해당 인덱스에 노드가 존재하지 않으면 새 노드를 바로 추가
	if (hashTable->table[index] == NULL) {
		hashTable->table[index] = newNode;
	} else {
		// 체이닝 처리: 해당 인덱스의 리스트 끝으로 이동
		HashNode* current = hashTable->table[index];
		while (current->next_node != NULL) {
			current = current->next_node; // 다음 노드로 이동
		}
		// 리스트 끝에 새 노드 추가
		current->next_node = newNode;
	}

	// pthread_rwlock_unlock(&hashTable->lock);
	return true;
}

// 키로 값을 검색하는 함수
struct client_r_info* searchByKey(HashTable* hashTable, int key)
{
	// pthread_rwlock_rdlock(&hashTable->lock);
	unsigned int index = hashFunction(key);
	HashNode* node = hashTable->table[index];

	// 해당 키가 존재하는지 확인
	if (node != NULL && node->key == key) {
		// pthread_rwlock_unlock(&hashTable->lock);
		return node->value;
	}

	// pthread_rwlock_unlock(&hashTable->lock);
	return NULL; // 값을 찾을 수 없으면 -1 반환
}

// 값으로 키를 검색하는 함수
int searchByValue(HashTable* hashTable, struct client_r_info *value)
{
	// pthread_rwlock_rdlock(&hashTable->lock);
	for (int i = 0; i < hashTable->size; i++) {
		HashNode* node = hashTable->table[i];
		if (node != NULL && node->value == value) {
			// pthread_rwlock_unlock(&hashTable->lock);
			return node->key;  // 값에 해당하는 키를 반환
		}
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	return -1; // 키를 찾을 수 없으면 -1 반환
}

// 키로 값을 삭제하는 함수
int deleteByKey(HashTable* hashTable, int key)
{
	// pthread_rwlock_wrlock(&hashTable->lock);
	unsigned int index = hashFunction(key);
	HashNode* node = hashTable->table[index];
	HashNode* prev = NULL;

	// 해당 인덱스에 노드가 없으면
	if (node == NULL) {
		errorInfoMes("Key not found!\n");
		return;
	}

	// 리스트 탐색
	while (node != NULL) {
		if (node->key == key) {
			// 삭제할 노드가 첫 번째 노드인 경우
			if (prev == NULL) {
				hashTable->table[index] = node->next_node;
			} else {
			// 중간 또는 마지막 노드인 경우
				prev->next_node = node->next_node;
			}

			// 노드 메모리 해제
			free(node);
			debugMes("Key %d deleted successfully.\n", key);
			// pthread_rwlock_unlock(&hashTable->lock);
			return 1;
		}

		// 다음 노드로 이동
		prev = node;
		node = node->next_node;
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Value not found!\n");
	return -1;
}

// 값으로 값을 삭제하는 함수
void deleteByValue(HashTable* hashTable, struct client_r_info *value)
{
	// pthread_rwlock_wrlock(&hashTable->lock);
	for (int i = 0; i < hashTable->size; i++) {
		HashNode* node = hashTable->table[i];
		HashNode* prev = NULL;

		// 인덱스 내 연결 리스트 탐색
		while (node != NULL) {
			if (node->value == value) {
				// 삭제할 노드가 첫 번째 노드인 경우
				if (prev == NULL) {
					hashTable->table[i] = node->next_node;
				} else {
					// 중간 또는 마지막 노드인 경우
					prev->next_node = node->next_node;
				}

				// 노드 메모리 해제
				free(node);
				// pthread_rwlock_unlock(&hashTable->lock);
				debugMes("Value deleted successfully.\n");
				return;
			}

			// 다음 노드로 이동
			prev = node;
			node = node->next_node;
		}
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Value not found!\n");
}

// 해시 테이블 메모리 해제 함수
void freeHashTable(HashTable* hashTable) {
        for (int i = 0; i < hashTable->size; i++) {
                if (hashTable->table[i] != NULL) {
                        free(hashTable->table[i]);
                }
        }
		// pthread_rwlock_destroy(&hashTable->lock);
        free(hashTable);
}


/* for SocketNode */

// 해시 테이블 생성 함수
SocketHashTable* createSocketHashTable()
{
	SocketHashTable* newTable = (SocketHashTable*)malloc(sizeof(SocketHashTable));
	newTable->size = HT_SZ;

	// 테이블을 NULL로 초기화
	for (int i = 0; i < HT_SZ ; i++) {
		newTable->table[i] = NULL;
	}

	// pthread_rwlock_init(&newTable->lock, NULL);
	return newTable;
}

// 키와 값을 입력받아 해시 테이블에 삽입하는 함수
bool socket_insertByValue(SocketHashTable* hashTable, int key, struct socket_info *value)
{
	// pthread_rwlock_wrlock(&hashTable->lock);

	// 새로운 노드 생성
	SocketNode* newNode = (SocketNode*)malloc(sizeof(SocketNode));
	if (newNode == NULL) {
		// pthread_rwlock_unlock(&hashTable->lock);
		errorInfoMes("Fail to malloc\n");
		return false; // 메모리 할당 실패
	}

	unsigned int index = hashFunction(key);

	newNode->key = key;
	newNode->value = value;
	newNode->index = index;
	newNode->next_node = NULL; // 초기화


	// 해당 인덱스에 노드가 존재하지 않으면 새 노드를 바로 추가
	if (hashTable->table[index] == NULL) {
		hashTable->table[index] = newNode;
	} else {
		// 체이닝 처리: 해당 인덱스의 리스트 끝으로 이동
		SocketNode* current = hashTable->table[index];
		while (current->next_node != NULL) {
			current = current->next_node; // 다음 노드로 이동
		}
		// 리스트 끝에 새 노드 추가
		current->next_node = newNode;
	}

	// pthread_rwlock_unlock(&hashTable->lock);
	return true;
}

// 키로 값을 검색하는 함수
struct socket_info* socket_searchByKey(SocketHashTable* hashTable, int key)
{
	// pthread_rwlock_rdlock(&hashTable->lock);
	unsigned int index = hashFunction(key);
	SocketNode* node = hashTable->table[index];

	// 해당 키가 존재하는지 확인
	if (node != NULL && node->key == key) {
		// pthread_rwlock_unlock(&hashTable->lock);
		return node->value;
	}

	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Fail to find value in HT\n");
	return NULL; // 값을 찾을 수 없으면 -1 반환
}

// 값으로 키를 검색하는 함수
int socket_searchByValue(SocketHashTable* hashTable, struct socket_info *value)
{
	// pthread_rwlock_rdlock(&hashTable->lock);
	for (int i = 0; i < hashTable->size; i++) {
		SocketNode* node = hashTable->table[i];
		if (node != NULL && node->value == value) {
			// pthread_rwlock_unlock(&hashTable->lock);
			return node->key;  // 값에 해당하는 키를 반환
		}
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Fail to find key in HT\n");
	return -1; // 키를 찾을 수 없으면 -1 반환
}

// 키로 값을 삭제하는 함수
void socket_deleteByKey(SocketHashTable* hashTable, int key)
{
	// pthread_rwlock_wrlock(&hashTable->lock);
	unsigned int index = hashFunction(key);
	SocketNode* node = hashTable->table[index];
	SocketNode* prev = NULL;

	// 해당 인덱스에 노드가 없으면
	if (node == NULL) {
		errorInfoMes("Key not found!\n");
		return;
	}

	// 리스트 탐색
	while (node != NULL) {
		if (node->key == key) {
			// 삭제할 노드가 첫 번째 노드인 경우
			if (prev == NULL) {
				hashTable->table[index] = node->next_node;
			} else {
			// 중간 또는 마지막 노드인 경우
				prev->next_node = node->next_node;
			}

			// 노드 메모리 해제
			free(node);
			debugMes("Key %d deleted successfully.\n", key);
			// pthread_rwlock_unlock(&hashTable->lock);
			return;
		}

		// 다음 노드로 이동
		prev = node;
		node = node->next_node;
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Value not found!\n");
}

// 값으로 값을 삭제하는 함수
void socket_deleteByValue(SocketHashTable* hashTable, struct socket_info *value)
{
	// pthread_rwlock_wrlock(&hashTable->lock);
	for (int i = 0; i < hashTable->size; i++) {
		SocketNode* node = hashTable->table[i];
		SocketNode* prev = NULL;

		// 인덱스 내 연결 리스트 탐색
		while (node != NULL) {
			if (node->value == value) {
				// 삭제할 노드가 첫 번째 노드인 경우
				if (prev == NULL) {
					hashTable->table[i] = node->next_node;
				} else {
					// 중간 또는 마지막 노드인 경우
					prev->next_node = node->next_node;
				}

				// 노드 메모리 해제
				free(node);
				// pthread_rwlock_unlock(&hashTable->lock);
				debugMes("Value deleted successfully.\n");
				return;
			}

			// 다음 노드로 이동
			prev = node;
			node = node->next_node;
		}
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Value not found!\n");
}

// 해시 테이블 메모리 해제 함수
void socket_freeHashTable(SocketHashTable* hashTable)
{
        for (int i = 0; i < hashTable->size; i++) {
                if (hashTable->table[i] != NULL) {
                        free(hashTable->table[i]);
                }
        }
		// pthread_rwlock_destroy(&hashTable->lock);
        free(hashTable);
}

/* for FDNode */

// 해시 테이블 생성 함수
FDHashTable* createFDHashTable()
{
	FDHashTable* newTable = (FDHashTable*)malloc(sizeof(FDHashTable));
	newTable->size = HT_SZ;

	// 테이블을 NULL로 초기화
	for (int i = 0; i < HT_SZ ; i++) {
		newTable->table[i] = NULL;
	}

	// pthread_rwlock_init(&newTable->lock, NULL);
	return newTable;
}

// 키와 값을 입력받아 해시 테이블에 삽입하는 함수
bool fd_insertByValue(FDHashTable* hashTable, int key, int value)
{
	// pthread_rwlock_wrlock(&hashTable->lock);

	// 새로운 노드 생성
	FDNode* newNode = (FDNode*)malloc(sizeof(FDNode));
	if (newNode == NULL) {
		// pthread_rwlock_unlock(&hashTable->lock);
		errorInfoMes("Fail to malloc\n");
		return false; // 메모리 할당 실패
	}

	unsigned int index = hashFunction(key);

	newNode->key = key;
	newNode->value = value;
	newNode->index = index;
	newNode->next_node = NULL; // 초기화


	// 해당 인덱스에 노드가 존재하지 않으면 새 노드를 바로 추가
	if (hashTable->table[index] == NULL) {
		hashTable->table[index] = newNode;
	} else {
		// 체이닝 처리: 해당 인덱스의 리스트 끝으로 이동
		FDNode* current = hashTable->table[index];
		while (current->next_node != NULL) {
			current = current->next_node; // 다음 노드로 이동
		}
		// 리스트 끝에 새 노드 추가
		current->next_node = newNode;
	}

	// pthread_rwlock_unlock(&hashTable->lock);
	return true;
}

// 키로 값을 검색하는 함수
int fd_searchByKey(FDHashTable* hashTable, int key)
{
	// pthread_rwlock_rdlock(&hashTable->lock);
	unsigned int index = hashFunction(key);
	FDNode* node = hashTable->table[index];

	// 해당 키가 존재하는지 확인
	if (node != NULL && node->key == key) {
		// pthread_rwlock_unlock(&hashTable->lock);
		return node->value;
	}

	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Fail to find value in HT\n");
	return NULL; // 값을 찾을 수 없으면 -1 반환
}

// 값으로 키를 검색하는 함수
int fd_searchByValue(FDHashTable* hashTable, int value)
{
	// pthread_rwlock_rdlock(&hashTable->lock);
	for (int i = 0; i < hashTable->size; i++) {
		FDNode* node = hashTable->table[i];
		if (node != NULL && node->value == value) {
			// pthread_rwlock_unlock(&hashTable->lock);
			return node->key;  // 값에 해당하는 키를 반환
		}
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Fail to find key in HT\n");
	return -1; // 키를 찾을 수 없으면 -1 반환
}

// 키로 값을 삭제하는 함수
void fd_deleteByKey(FDHashTable* hashTable, int key)
{
	// pthread_rwlock_wrlock(&hashTable->lock);
	unsigned int index = hashFunction(key);
	FDNode* node = hashTable->table[index];
	FDNode* prev = NULL;

	// 해당 인덱스에 노드가 없으면
	if (node == NULL) {
		errorInfoMes("Key not found!\n");
		return;
	}

	// 리스트 탐색
	while (node != NULL) {
		if (node->key == key) {
			// 삭제할 노드가 첫 번째 노드인 경우
			if (prev == NULL) {
				hashTable->table[index] = node->next_node;
			} else {
			// 중간 또는 마지막 노드인 경우
				prev->next_node = node->next_node;
			}

			// 노드 메모리 해제
			free(node);
			debugMes("Key %d deleted successfully.\n", key);
			// pthread_rwlock_unlock(&hashTable->lock);
			return;
		}

		// 다음 노드로 이동
		prev = node;
		node = node->next_node;
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Value not found!\n");
}

// 값으로 값을 삭제하는 함수
void fd_deleteByValue(FDHashTable* hashTable, int value)
{
	// pthread_rwlock_wrlock(&hashTable->lock);
	for (int i = 0; i < hashTable->size; i++) {
		FDNode* node = hashTable->table[i];
		FDNode* prev = NULL;

		// 인덱스 내 연결 리스트 탐색
		while (node != NULL) {
			if (node->value == value) {
				// 삭제할 노드가 첫 번째 노드인 경우
				if (prev == NULL) {
					hashTable->table[i] = node->next_node;
				} else {
					// 중간 또는 마지막 노드인 경우
					prev->next_node = node->next_node;
				}

				// 노드 메모리 해제
				free(node);
				// pthread_rwlock_unlock(&hashTable->lock);
				debugMes("Value deleted successfully.\n");
				return;
			}

			// 다음 노드로 이동
			prev = node;
			node = node->next_node;
		}
	}
	// pthread_rwlock_unlock(&hashTable->lock);
	errorInfoMes("Value not found!\n");
}

// 해시 테이블 메모리 해제 함수
void fd_freeHashTable(FDHashTable* hashTable)
{
        for (int i = 0; i < hashTable->size; i++) {
                if (hashTable->table[i] != NULL) {
                        free(hashTable->table[i]);
                }
        }
		// pthread_rwlock_destroy(&hashTable->lock);
        free(hashTable);
}