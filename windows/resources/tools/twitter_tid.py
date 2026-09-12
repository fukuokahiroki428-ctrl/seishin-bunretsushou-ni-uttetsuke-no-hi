#!/usr/bin/env python3
"""Generate X-Client-Transaction-Id for Twitter API requests using twikit."""
import sys
import json
import asyncio

async def _load_init_args():
    """자격증명을 받는다.
    ★ 예전에는 JSON 을 argv 로 받았다. 윈도우에서 남의 프로세스 명령줄은 아무
      프로세스나 읽을 수 있어서(WMI Win32_Process, 작업 관리자의 '명령줄' 열)
      auth_token·ct0·앱 비밀번호가 그대로 노출됐다. 이제 stdin 으로 받는다.
      argv 경로는 손으로 시험할 때를 위해 남겨 두되, 그 쓰임은 노출된다."""
    if len(sys.argv) >= 2 and sys.argv[1] == "--stdin-args":
        return json.loads(sys.stdin.read())
    if len(sys.argv) >= 2:
        return json.loads(sys.argv[1])
    print(json.dumps({"error": "init args required (--stdin-args)"}))
    sys.exit(1)


def main():
    args = _load_init_args()
    auth_token = args["auth_token"]
    ct0 = args["ct0"]
    paths = args["paths"]  # list of API URL paths

    try:
        from twikit import Client
    except ImportError:
        print(json.dumps({"error": "twikit not installed"}))
        sys.exit(1)

    client = Client(language="en-US")
    client.set_cookies({
        "auth_token": auth_token,
        "ct0": ct0,
    })

    # Initialize client transaction (fetches homepage + ondemand JS)
    try:
        ct_headers = {
            'Accept-Language': 'en-US,en;q=0.9',
            'Cache-Control': 'no-cache',
            'Referer': 'https://x.com',
            'User-Agent': client._user_agent
        }
        await client.client_transaction.init(client.http, ct_headers)
    except Exception as e:
        print(json.dumps({"error": f"init failed: {str(e)}"}))
        sys.exit(1)

    # Generate transaction IDs for each path
    results = {}
    for path in paths:
        try:
            tid = client.client_transaction.generate_transaction_id(
                method="GET", path=path
            )
            results[path] = tid
        except Exception as e:
            results[path] = ""

    print(json.dumps(results))

if __name__ == "__main__":
    asyncio.run(main())
