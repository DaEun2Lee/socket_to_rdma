#include "hash_delee.h"

// 해시 함수 (키를 사용하여 해시 인덱스 생성)
unsigned int MurmurHash3(const void *key, int len, unsigned int seed) {
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

// 해시 테이블 생성자
HashTable::HashTable() {
	size = HT_SZ;

	for (int i = 0; i < HT_SZ; i++) {
		table[i] = nullptr;
	}

	pthread_rwlock_init(&lock, nullptr);
}

// 해시 테이블 소멸자
HashTable::~HashTable() {
	freeHashTable();
}

// 해시 함수 (키를 사용하여 해시 인덱스 생성)
unsigned int HashTable::hashFunction(int key) {
	return MurmurHash3(&key, sizeof(int), HT_SEED) % HT_SZ;
}

// 키와 값을 입력받아 해시 테이블에 삽입하는 함수
bool HashTable::insertByValue(int key, struct rdma_info *value) {
	pthread_rwlock_wrlock(&lock);

	unsigned int index = hashFunction(key);
	HashNode* newNode = new HashNode(key, value, index);

	if (!newNode) {
		pthread_rwlock_unlock(&lock);
		return false;
	}

	// 체이닝 방식 삽입
	if (table[index] == nullptr) {
		table[index] = newNode;
	} else {
		HashNode* current = table[index];
		while (current->next_node != nullptr) {
			current = current->next_node;
		}
		current->next_node = newNode;
	}

	pthread_rwlock_unlock(&lock);
	return true;
}

// 키로 값을 검색하는 함수
struct rdma_info* HashTable::searchByKey(int key) {
	pthread_rwlock_rdlock(&lock);

	unsigned int index = hashFunction(key);
	HashNode* node = table[index];

	while (node != nullptr) {
		if (node->key == key) {
			pthread_rwlock_unlock(&lock);
			return node->value;
		}
		node = node->next_node;
	}

	pthread_rwlock_unlock(&lock);
	return nullptr;
}

// 값으로 키를 검색하는 함수
int HashTable::searchByValue(struct rdma_info* value) {
	pthread_rwlock_rdlock(&lock);

	for (int i = 0; i < size; i++) {
		HashNode* node = table[i];
		while (node != nullptr) {
			if (node->value == value) {
				pthread_rwlock_unlock(&lock);
				return node->key;
			}
			node = node->next_node;
		}
	}

	pthread_rwlock_unlock(&lock);
	return -1;
}

// 키로 값을 삭제하는 함수
void HashTable::deleteByKey(int key) {
	pthread_rwlock_wrlock(&lock);

	unsigned int index = hashFunction(key);
	HashNode* node = table[index];
	HashNode* prev = nullptr;

	while (node != nullptr) {
		if (node->key == key) {
			if (prev == nullptr) {
				table[index] = node->next_node;
			} else {
				prev->next_node = node->next_node;
			}
			delete node;
			pthread_rwlock_unlock(&lock);
			return;
		}
		prev = node;
		node = node->next_node;
	}

	pthread_rwlock_unlock(&lock);
}

// 값으로 노드를 삭제하는 함수
void HashTable::deleteByValue(struct rdma_info* value) {
	pthread_rwlock_wrlock(&lock);

	for (int i = 0; i < size; i++) {
		HashNode* node = table[i];
		HashNode* prev = nullptr;

		while (node != nullptr) {
			if (node->value == value) {
				if (prev == nullptr) {
					table[i] = node->next_node;
				} else {
					prev->next_node = node->next_node;
				}
				delete node;
				pthread_rwlock_unlock(&lock);
				return;
			}
			prev = node;
			node = node->next_node;
		}
	}

	pthread_rwlock_unlock(&lock);
}

// 해시 테이블 메모리 해제 함수
void HashTable::freeHashTable() {
	pthread_rwlock_wrlock(&lock);

	for (int i = 0; i < size; i++) {
		HashNode* node = table[i];
		while (node != nullptr) {
			HashNode* temp = node;
			node = node->next_node;
			delete temp;
		}
		table[i] = nullptr;
	}

	pthread_rwlock_unlock(&lock);
	pthread_rwlock_destroy(&lock);
}


// r_info
// 해시 테이블 생성자
SocketNodeHashTable::SocketNodeHashTable() {
	size = HT_SZ;

	for (int i = 0; i < HT_SZ; i++) {
		table[i] = nullptr;
	}
	pthread_rwlock_init(&lock, nullptr);
}

// 해시 테이블 소멸자
SocketNodeHashTable::~SocketNodeHashTable() {
	freeHashTable();
}

// 해시 함수
unsigned int SocketNodeHashTable::hashFunction(int key) {
	return MurmurHash3(&key, sizeof(int), HT_SEED) % HT_SZ;
}

// 키와 값을 삽입하는 함수
bool SocketNodeHashTable::insertByValue(int key, struct socket_info *value) {
	pthread_rwlock_wrlock(&lock);

	unsigned int index = hashFunction(key);
	SocketNode* newNode = new SocketNode(key, value, index);
	if (!newNode) {
		pthread_rwlock_unlock(&lock);
		return false;
	}

	if (table[index] == nullptr) {
		table[index] = newNode;
	} else {
		SocketNode* current = table[index];
//		while (current->next_node != nullptr) {

		while (true){

			if(current->next_node == nullptr){
				break;
			} else {
				current = current->next_node;
			}
		}
		current->next_node = newNode;
	}

	pthread_rwlock_unlock(&lock);
	return true;
}

// 키로 검색하는 함수
struct socket_info* SocketNodeHashTable::searchByKey(int key) {
	pthread_rwlock_rdlock(&lock);
	unsigned int index = hashFunction(key);
	SocketNode* node = table[index];
	while (node != nullptr) {
		if (node->key == key) {
			pthread_rwlock_unlock(&lock);
			return static_cast<SocketNode*>(node)->value;
		}
		node = node->next_node;
	}

	pthread_rwlock_unlock(&lock);
	return nullptr;
}

// 값으로 키를 검색하는 함수
int SocketNodeHashTable::searchByValue(struct socket_info* value) {
	pthread_rwlock_rdlock(&lock);
	for (int i = 0; i < size; i++) {
		SocketNode* node = table[i];
		while (node != nullptr) {
			if (static_cast<SocketNode*>(node)->value == value) {
				pthread_rwlock_unlock(&lock);
				return node->key;
			}
			node = node->next_node;
		}
	}

	pthread_rwlock_unlock(&lock);
	return -1;
}

// 키로 노드를 삭제하는 함수
void SocketNodeHashTable::deleteByKey(int key) {
	pthread_rwlock_wrlock(&lock);
	unsigned int index = hashFunction(key);
	SocketNode* node = table[index];
	SocketNode* prev = nullptr;
	while (node != nullptr) {
		if (node->key == key) {
			if (prev == nullptr) {
				table[index] = node->next_node;
			} else {
				prev->next_node = node->next_node;
			}
			delete static_cast<SocketNode*>(node);
			pthread_rwlock_unlock(&lock);
			return;
		}
		prev = node;
		node = node->next_node;
	}

	pthread_rwlock_unlock(&lock);
}

// 값으로 노드를 삭제하는 함수
void SocketNodeHashTable::deleteByValue(struct socket_info* value) {
	pthread_rwlock_wrlock(&lock);
	for (int i = 0; i < size; i++) {
		SocketNode* node = table[i];
		SocketNode* prev = nullptr;
		while (node != nullptr) {
			if (static_cast<SocketNode*>(node)->value == value) {
				if (prev == nullptr) {
					table[i] = node->next_node;
				} else {
					prev->next_node = node->next_node;
				}
				delete static_cast<SocketNode*>(node);
				pthread_rwlock_unlock(&lock);
				return;
			}
			prev = node;
			node = node->next_node;
		}
	}

	pthread_rwlock_unlock(&lock);
}

// 해시 테이블 메모리 해제 함수
void SocketNodeHashTable::freeHashTable() {
	pthread_rwlock_wrlock(&lock);
	for (int i = 0; i < size; i++) {
		SocketNode* node = table[i];
		while (node != nullptr) {
			SocketNode* temp = node;
			node = node->next_node;
			delete static_cast<SocketNode*>(temp);
		}
		table[i] = nullptr;
	}

	pthread_rwlock_unlock(&lock);
	pthread_rwlock_destroy(&lock);
}

