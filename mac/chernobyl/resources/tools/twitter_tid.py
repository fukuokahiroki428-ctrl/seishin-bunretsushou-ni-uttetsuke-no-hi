#!/usr/bin/env python3
"""Generate X-Client-Transaction-Id for Twitter API requests using twikit."""
import sys
import json
import asyncio

async def main():
    args = json.loads(sys.argv[1])
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
