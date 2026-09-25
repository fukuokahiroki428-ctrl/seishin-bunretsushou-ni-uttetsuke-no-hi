# 고침 꾸러미 (1단계 — 데이터만)

앱이 공개 저장소 `hotfix-mac` 브랜치의 `hotfix/` 에서 받아 쓰는 값이다. 새 판을 굽지 않고 고칠 수 있다.

| 파일 | 내용 | 앱이 받는 모양 |
|---|---|---|
| `api_overrides.json` | 질의 번호·해시 | `twitter.*` 는 `/i/api/graphql/<해시>/<이름>` 경로만, `twitter.bearer`, `instagram.*DocId` 는 숫자, `instagram.*Providers` 는 provider 이름 목록 |
| `shape_aliases.json` | 응답 이름 별명 `{"posts": ["records"]}` | 영숫자·밑줄만 |
| `repair_rules.json` | 수리 도우미 규칙 | 무늬는 글자 그대로의 대안(`가|나`)만, 할 일은 `refreshAllTokens`·`updateModules`·`repairPython`·빈칸만, 글에 링크 금지 |
| `hotfix.json` | 판 번호·최소 앱 판·한 줄 설명 | |

올리는 법: `python3 tools/hotfix_publish.py` → 검사·개인정보 훑기·manifest 만들기·`hotfix-mac` 에 커밋. 밀기는 따로.
판 번호(`version`)를 올려야 앱이 새로 받는다. 받은 것은 실행되지 않는다.
