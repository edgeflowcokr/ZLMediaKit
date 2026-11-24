/*
 * EdgeFlowAI: 키프레임만 라이브 갈래(2026-11-24).
 * 「<원본 스트림>~key」 를 요청하면 원본 영상 트랙의 I프레임(+SPS/PPS 등 설정 프레임)만 골라 새 스트림으로 내보낸다.
 * 다분할 화면에서 브라우저 디코드 여유가 모자랄 때 서브 스트림이 없는 채널도 낮은 부담으로 보이게 하는 용도다.
 * 시청자가 없거나 원본이 사라지면 스스로 닫는다. 녹화·HLS 저장은 하지 않는다.
 */
#ifndef ZLMEDIAKIT_KEYFRAMERELAY_H
#define ZLMEDIAKIT_KEYFRAMERELAY_H

void installKeyframeRelay();

#endif // ZLMEDIAKIT_KEYFRAMERELAY_H
