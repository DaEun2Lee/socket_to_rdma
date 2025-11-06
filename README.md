# socket_to_rdma

## 📌1. 프로젝트 개요

**`socket_to_rdma`**는 표준 TCP 소켓(Socket) API를 사용하는 기존 네트워크 애플리케이션의 통신을 **RDMA(Remote Direct Memory Access)**로 투명하게(transparently) 가속하는 라이브러리입니다.

애플리케이션의 소스 코드를 수정하지 않고도, `LD_PRELOAD` 트릭을 사용하여 소켓 함수 호출(예: `send`, `recv`)을 가로채어 RDMA 연산으로 대체 수행합니다. 이를 통해 기존 프로그램의 코드 변경 없이 RDMA가 제공하는 초저지연, 고대역폭 네트워킹의 이점을 누릴 수 있습니다.


## 📌2. 프로젝트 특징

- 소켓 기반 코드(리눅스) → RDMA 기반 코드로의 전환 예시 제공
- 간단한 송수신 흐름 코드 포함 (호스트 측 host/, 스마트NIC 측 snic/ 디렉토리 존재)
- RDMA 메모리 등록, 큐페어 생성, 보내기/받기(Post Send/Recv) 등의 기본 동작 구현
- 확장 및 커스터마이즈 하기 쉬운 구조
- 학습용으로 적합 (실제 상용 통신 라이브러리보다는 구조 이해 위주)


## 📌3. 설치 및 빌드

**[Ubuntu/Debian 기준 설치 예시]**

###  1. 요구 사항 (Dependencies) 
```bash
sudo apt-get update
sudo apt-get install build-essential gcc make
sudo apt-get install libibverbs-dev librdmacm-dev
```

### 2. 리포지토리 클론
```bash
git clone [https://github.com/DaEun2Lee/socket_to_rdma.git](https://github.com/DaEun2Lee/socket_to_rdma.git)
cd socket_to_rdma
```

### 3. snic 라이브러리 빌드
```bash
cd snic
make
cd ..
```

### 4. host 예제 애플리케이션 빌드
```bash
cd host
make
cd ..
```

### 5. 사용 방법
LD_PRELOAD 환경 변수를 사용하여, host의 애플리케이션 실행 시 snic 라이브러리를 먼저 로드하도록 지정합니다.

1. 서버 실행
```bash
# (서버 터미널)
export LD_PRELOAD=./snic/libsnic.so
./host/server
```

2. 클라이언트 실행
```bash
# (클라이언트 터미널)
export LD_PRELOAD=./snic/libsnic.so ./host/client [서버 IP 주소]
```

실행 확인
LD_PRELOAD를 사용하지 않고 실행하면, host 프로그램은 표준 TCP 소켓을 통해 통신합니다.
LD_PRELOAD를 사용하여 실행하면, 프로그램의 출력은 동일하지만 실제 네트워크 트래픽은 snic 라이브러리에 의해 RDMA로 처리됩니다. (예: ibstat 또는 성능 모니터링 툴로 확인 가능)

## 6. 향후 계획 (To-Do)
[ ] 더 많은 소켓 API 및 fcntl 옵션 지원
[ ] 비동기(non-blocking) 소켓 모드 지원 강화
[ ] epoll / select 등 I/O 멀티플렉싱 함수 후킹
[ ] 성능 벤치마크 및 최적화
[ ] 예외 처리 및 오류 복구 로직 강화

# Authors

## Author of the original code:
	Animesh Trivedi
	atrivedi@apache.org
## Author: 
	Daeun Lee
	delee0929@gmail.com
