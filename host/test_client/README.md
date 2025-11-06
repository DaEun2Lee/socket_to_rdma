# client_multi

## 📌 프로그램 설명
`client_multi`는 멀티스레드 기반 HTTP 클라이언트 프로그램입니다.  
여러 개의 스레드를 생성해 동시에 서버에 요청을 보내고, 각 요청의 **지연 시간(latency)** 및 **처리 속도(throughput)** 를 측정합니다.  
HTTP Keep-Alive 여부를 설정할 수 있어, 요청마다 새로운 연결을 생성하거나 하나의 연결을 재사용하는 실험이 가능합니다.  


---

## 📌 실행 방법
 ```bash
./client_multi<requests_per_conn> <keep_alive: 0|1>
 ```

### 인자 설명
- **`requests_per_conn`** : 각 스레드(연결)에서 보낼 요청 횟수  
- **`keep_alive`** : HTTP Keep-Alive 사용 여부  
  - `0` → 요청마다 연결을 새로 맺음 (`Connection: close`)  
  - `1` → 같은 연결에서 여러 요청 전송 (`Connection: keep-alive`)  

📌 전체 요청 개수 = `requests_per_conn`  

### 실행 예시
- **10개 스레드, 각 5회 요청 (총 50회), Keep-Alive 사용**
  ```bash
  ./client_multi 10 5 1
  ```


## 📌 출력 결과
실행이 끝나면 client.log 파일에 다음과 같은 내용이 저장됩니다:
- 각 요청별 지연 시간(latency) 및 처리 속도(throughput)
- 최종 평균 지연 시간 및 평균 처리 속도

### 예시:
  ```bash
  Conn[0] Req[1]: Latency=12.34 ms, Throughput=1000000.00 Bps
  === Final Average Latency: 15.67 ms, Final Average Throughput: 950000.00 Bps ===
  ```

## 📌 컴파일 방법
  ```bash
  gcc -o client_multi client_multi.c -Wall -O -lpthread -lm
  ```
