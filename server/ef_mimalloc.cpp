// EdgeFlowAI: 윈도 MediaServer 의 C++ new/delete 를 mimalloc(스레드별 힙)으로 바꾼다.
//   기본 프로세스 힙은 잠금 하나를 모든 스레드가 공유한다. 0.20(120채널, 소스 240)에서 이벤트 스레드 32개 중 28~30개가 변환기 패킷
//   할당·해제의 힙 잠금에 줄을 서 100~120초마다 몇 초씩 수신이 끊겼다(정지 순간 스택 덤프, EdgeFlowAI docs/MEDIA_THREAD_LOAD.md §5).
//   new/delete 교체는 이 파일 하나에서만 한다(mimalloc-new-delete.h 규약). C 코드의 malloc/free 는 그대로다.
#if defined(_WIN32) && defined(EF_MEDIA_MIMALLOC)
#include <mimalloc-new-delete.h>
#endif
