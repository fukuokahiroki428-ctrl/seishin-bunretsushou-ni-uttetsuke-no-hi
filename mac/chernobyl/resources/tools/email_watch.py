#!/usr/bin/env python3
"""
email_watch.py — IMAP 새 메일 감지 (내각회 알림 트리거용)

호출:
  email_watch.py --stdin-args
      → stdin 으로 JSON 한 줄을 받는다:
        {"server":"...","port":993,"user":"...","password":"...",
         "filter_from":"","filter_subject":"","last_uid":0}
  email_watch.py <server> <port> <user> <password> [filter_from] [filter_subject] [last_uid]
      → 손으로 시험할 때만. ★ 이 길로 부르면 비밀번호가 명령줄에 남고,
        윈도우에서는 아무 프로세스나 그것을 읽을 수 있다.

stdout: JSON {found: bool, count: N, last_uid: M, samples: [{from, subject, date}]}
        매치된 미읽음 메일이 있으면 found=true. backend가 그 신호 받고 내각회 즉시 실행.

필터:
  filter_from: 발신자 substring (대소문자 무시) — 비어있으면 모든 발신자
  filter_subject: 제목 substring (대소문자 무시) — 비어있으면 모든 제목
  last_uid: 이전에 본 마지막 UID — 그보다 큰 UID만 매치
"""
import sys
import json
import imaplib
import email
from email.header import decode_header


def decode_str(s):
    if not s:
        return ""
    parts = decode_header(s)
    out = []
    for text, enc in parts:
        if isinstance(text, bytes):
            try:
                out.append(text.decode(enc or "utf-8", errors="replace"))
            except Exception:
                out.append(text.decode("utf-8", errors="replace"))
        else:
            out.append(text)
    return "".join(out)


def _read_init_line():
    """첫 줄을 원시 fd 에서 한 바이트씩 읽는다.

    ★ 파이썬의 버퍼는 한 번에 여러 줄을 삼킨다. 이 도구는 지금 한 줄만 받지만,
      나중에 명령을 이어 받게 바꿔도 첫 줄 뒤가 사라지지 않도록 다른 데몬들과
      같은 방식으로 맞춰 둔다.
    """
    import os as _os
    buf = b""
    while True:
        ch = _os.read(0, 1)
        if not ch or ch == b"\n":
            break
        buf += ch
    return buf.decode("utf-8", "replace")


def _load_args():
    """자격증명을 받는다.

    ★ 예전에는 서버·계정·비밀번호를 argv 로 받았다. 윈도우에서 남의 프로세스
      명령줄은 아무 프로세스나 읽을 수 있어서(WMI Win32_Process, 작업 관리자의
      '명령줄' 열) 메일 비밀번호가 그대로 노출됐다. 이제 stdin 으로 받는다.
      argv 길은 손으로 시험할 때를 위해 남겨 두되, 그 쓰임은 노출된다.
    """
    if len(sys.argv) >= 2 and sys.argv[1] == "--stdin-args":
        line = _read_init_line()
        if not line.strip():
            print(json.dumps({"error": "init args not received on stdin"}))
            sys.exit(2)
        a = json.loads(line)
        return (a.get("server", ""), int(a.get("port", 993) or 993),
                a.get("user", ""), a.get("password", ""),
                str(a.get("filter_from", "") or "").strip().lower(),
                str(a.get("filter_subject", "") or "").strip().lower(),
                int(a.get("last_uid", 0) or 0))
    if len(sys.argv) < 5:
        print(json.dumps({"error": "usage: --stdin-args (or: server port user pass [from] [subj] [uid])"}))
        sys.exit(2)
    return (sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4],
            (sys.argv[5] if len(sys.argv) > 5 else "").strip().lower(),
            (sys.argv[6] if len(sys.argv) > 6 else "").strip().lower(),
            int(sys.argv[7]) if len(sys.argv) > 7 and sys.argv[7].isdigit() else 0)


def main():
    (server, port, user, password,
     filter_from, filter_subject, last_uid) = _load_args()

    try:
        mail = imaplib.IMAP4_SSL(server, port, timeout=15)
        mail.login(user, password)
        mail.select("INBOX", readonly=True)

        # UID 검색 — 마지막 본 UID 이후의 모든 메일
        criteria = f"UID {last_uid + 1}:*" if last_uid > 0 else "UNSEEN"
        status, data = mail.uid("SEARCH", None, criteria)
        if status != "OK":
            print(json.dumps({"error": f"search failed: {status}"}))
            sys.exit(1)

        uids = data[0].split() if data and data[0] else []
        samples = []
        max_uid = last_uid
        match_count = 0

        for uid in uids[-30:]:  # 최근 30개만 검사 (과도한 fetch 방지)
            uid_int = int(uid)
            if uid_int > max_uid:
                max_uid = uid_int
            status, msg_data = mail.uid("FETCH", uid, "(BODY.PEEK[HEADER])")
            if status != "OK" or not msg_data or not msg_data[0]:
                continue
            raw = msg_data[0][1]
            msg = email.message_from_bytes(raw)
            sender = decode_str(msg.get("From", ""))
            subject = decode_str(msg.get("Subject", ""))
            date = msg.get("Date", "")

            # 필터 매칭
            sender_l = sender.lower()
            subject_l = subject.lower()
            from_ok = (not filter_from) or (filter_from in sender_l)
            subj_ok = (not filter_subject) or (filter_subject in subject_l)
            if from_ok and subj_ok:
                match_count += 1
                samples.append({"uid": uid_int, "from": sender, "subject": subject, "date": date})

        mail.logout()

        result = {
            "found": match_count > 0,
            "count": match_count,
            "last_uid": max_uid,
            "samples": samples[-5:],  # 최근 5개만 보고
        }
        print(json.dumps(result, ensure_ascii=False))
        sys.exit(0)
    except imaplib.IMAP4.error as e:
        print(json.dumps({"error": f"imap_auth: {e}"}))
        sys.exit(1)
    except Exception as e:
        print(json.dumps({"error": f"{type(e).__name__}: {e}"}))
        sys.exit(1)


if __name__ == "__main__":
    main()
