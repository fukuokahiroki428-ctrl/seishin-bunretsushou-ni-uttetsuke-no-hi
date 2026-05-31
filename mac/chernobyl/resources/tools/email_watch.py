#!/usr/bin/env python3
"""
email_watch.py — IMAP 새 메일 감지 (내각회 알림 트리거용)

호출:
  email_watch.py <server> <port> <user> <password> [filter_from] [filter_subject] [last_uid]

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


def main():
    if len(sys.argv) < 5:
        print(json.dumps({"error": "usage: server port user pass [from_filter] [subj_filter] [last_uid]"}))
        sys.exit(2)
    server = sys.argv[1]
    port = int(sys.argv[2])
    user = sys.argv[3]
    password = sys.argv[4]
    filter_from = (sys.argv[5] if len(sys.argv) > 5 else "").strip().lower()
    filter_subject = (sys.argv[6] if len(sys.argv) > 6 else "").strip().lower()
    last_uid = int(sys.argv[7]) if len(sys.argv) > 7 and sys.argv[7].isdigit() else 0

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
