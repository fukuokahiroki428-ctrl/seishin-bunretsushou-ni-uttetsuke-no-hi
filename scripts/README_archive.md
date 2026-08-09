# 산출물 질의응답 (색인 + AI + 디스코드 봇)

수집해 둔 파일을 색인해 두고, 질문하면 근거와 함께 답한다.

## 쓰는 순서

**1) 색인 만들기** — 처음 한 번, 이후엔 새로 받은 것만 자동으로 추가된다.
```bash
python3 archive_index.py /저장/경로
```
- 증분이라 여러 번 돌려도 안전하다(경로+크기+수정시각이 같으면 건너뜀).
- Ctrl+C 로 끊고 다시 돌리면 이어서 한다.
- 색인 위치: `~/Library/Application Support/Miyo/Predormition/archive_index.db`

**2) 질문하기** — 앱에서 로컬 AI 를 켠 뒤.
```bash
python3 archive_ask.py "vivoさん 그림 뭐 있어?"
python3 archive_ask.py --search "先生虐待"        # AI 없이 검색만
```

**3) 디스코드 봇** (선택)
```bash
export DISCORD_BOT_TOKEN='...'      # ★ 파일에 적지 말 것
python3 archive_bot.py
```
디스코드에서 `@봇 질문` / `!ask 질문` / `!find 검색어` / `!status`

디스코드 개발자 포털에서 **MESSAGE CONTENT INTENT** 를 켜야 봇이 메시지를 읽는다.

## 무엇을 근거로 답하나

| 자료 | 읽는 것 |
|---|---|
| 이미지 | EXIF — 작가·제목·원본 URL·날짜 (수집할 때 앱이 심어 둔 것) |
| 엑셀 | 작가별 작품 목록의 행 (ID·제목·작가·유형 등) |
| JSON/TXT/HTML | 본문 텍스트 |
| 전부 | 파일명·폴더 구조·크기·날짜 |

## 한계 — 분명히 해둘 것

- **그림 내용은 못 본다.** 번들 모델(Qwen2.5)이 글자 전용이다. "빨간 옷 입은 사람 찾아줘"
  같은 질문은 안 된다. 나중에 비전 모델(Qwen2-VL 등)을 붙이면 가능해지며,
  색인 표에 `vision_desc` 칸을 비워 두었다.
- **색인에 없는 파일은 답에 안 나온다.** 새로 수집했으면 색인을 다시 돌린다.
- 일본어 자료가 많아 색인 토큰화를 `trigram` 으로 했다. 3글자 이상 부분검색이
  되지만 2글자 이하는 trigram 원리상 안 걸려 LIKE 로 보완한다.
