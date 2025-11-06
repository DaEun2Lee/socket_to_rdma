# Epoll 기반 고성능 HTTP 서버 (http_epoll_fixed.c)

이 프로그램은 C로 작성된 고성능, 이벤트 기반 웹 서버입니다. `epoll` (ET, 엣지 트리거), 논블로킹 I/O, `sendfile()`을 사용하여 높은 동시성 처리에 최적화되어 있습니다.

---

## 📌 컴파일 방법
```bash
    gcc -O2 -o http_epoll_fixed http_epoll_fixed.c
```
- 참고: 코드가 _GNU_SOURCE를 사용하여 strcasestr 함수를 사용합니다.

## 📌 실행 방법
 ```bash
    ./http_epoll_fixed [포트] [문서 루트 디렉토리]
 ```

- 서버는 포트 번호와 문서 루트 디렉토리를 반드시 명령줄 인자로 받아야 합니다.
- 예시:
 ```bash
    ./http_epoll_fixed 8080 /var/www/my_website
 ```
- 위 명령은 8080 포트에서 서버를 실행하며, `/var/www/my_website` 경로를 기준으로 정적 파일을 제공합니다.
- `GET /` 요청 → `/var/www/my_website/index.html` 반환 (경로 파싱 로직에 따라)
- `GET /image.png` 요청 → `/var/www/my_website/image.png` 반환


## 📌 기능
### 이벤트 기반 아키텍처
- Epoll (Edge-Triggered): `epoll`을 엣지 트리거(`EPOLLET`) 모드로 사용하여 I/O 이벤트를 효율적으로 관리합니다.
- 논블로킹 I/O: 리스닝 소켓과 클라이언트 소켓 모두 `O_NONBLOCK` 플래그를 설정하여 논블로킹으로 동작합니다.

### 고성능 파일 전송
- sendfile(): `sendfile()` 시스템 콜을 사용하여 커널 공간에서 사용자 공간으로의 불필요한 데이터 복사 없이 파일을 전송합니다. (Zero-Copy)

### HTTP/1.1 처리
- Keep-Alive: `Connection: keep-alive` 및 `Connection: close` 헤더를 파싱하여 HTTP Keep-Alive 연결을 지원합니다.
- 요청 파이프라이닝 (기본): 단일 읽기 버퍼에서 여러 HTTP 요청을 순차적으로 처리하는 `process_requests` 로직이 포함되어 있습니다.

### 동적 버퍼 관리
- 클라이언트별 읽기(`rbuf`) 및 쓰기(`wbuf`) 버퍼를 가지며, 필요에 따라 `realloc`을 통해 버퍼 크기를 동적으로 확장합니다.

### 정적 파일 및 에러 처리
- MIME 타입: `.html`, `.css`, `.js`, `.jpg`, `.png` 등 파일 확장자에 따라 기본적인 `Content-Type`을 설정합니다.
- 보안: `..` 문자열을 필터링하여 디렉토`리 역참조(Directory Traversal) 공격을 방지합니다.
- 에러 응답: 파일을 찾을 수 없을 때 `404 Not Found` 응답을 전송합니다.
