#!/usr/bin/env python3
"""Twitter API proxy using twikit - delegates all HTTP to twikit for proper TID/headers."""
import sys
import json
import asyncio


async def main():
    args = json.loads(sys.argv[1])

    try:
        from twikit import Client
        from twikit.client.gql import Endpoint, FEATURES, USER_FEATURES, flatten_params
    except ImportError:
        print(json.dumps({"error": "twikit not installed"}))
        sys.exit(1)

    client = Client(language="en-US")
    client.set_cookies({
        "auth_token": args["auth_token"],
        "ct0": args["ct0"],
    })

    # Initialize client transaction for automatic TID generation
    try:
        ct_headers = {
            'Accept-Language': 'en-US,en;q=0.9',
            'Cache-Control': 'no-cache',
            'Referer': 'https://x.com',
            'User-Agent': client._user_agent,
        }
        await client.client_transaction.init(client.http, ct_headers)
    except Exception as e:
        print(json.dumps({"error": f"TID init failed: {str(e)}"}))
        sys.exit(1)

    action = args["action"]

    try:
        if action == "user_by_screen_name":
            variables = {
                "screen_name": args["screen_name"],
                "withSafetyModeUserFields": False,
            }
            params = flatten_params({
                'variables': variables,
                'features': USER_FEATURES,
                'fieldToggles': {"withAuxiliaryUserLabels": False},
            })
            data, response = await client.request(
                'GET', Endpoint.USER_BY_SCREEN_NAME,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            print(json.dumps({
                "status": response.status_code,
                "body": data if response.status_code == 200 else response.text
            }))

        elif action == "user_tweets":
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 20),
                "includePromotedContent": True,
                "withQuickPromoteEligibilityTweetFields": True,
                "withVoice": True,
                "withV2Timeline": True,
            }
            cursor = args.get("cursor", "")
            if cursor:
                variables["cursor"] = cursor

            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.USER_TWEETS,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            print(json.dumps({
                "status": response.status_code,
                "body": data if response.status_code == 200 else response.text
            }))

        elif action == "search":
            variables = {
                "rawQuery": args["query"],
                "count": args.get("count", 20),
                "querySource": "typed_query",
                "product": "Latest",
            }
            cursor = args.get("cursor", "")
            if cursor:
                variables["cursor"] = cursor

            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.SEARCH_TIMELINE,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            print(json.dumps({
                "status": response.status_code,
                "body": data if response.status_code == 200 else response.text
            }))

        elif action == "likes":
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 20),
                "includePromotedContent": True,
                "withQuickPromoteEligibilityTweetFields": True,
                "withVoice": True,
                "withV2Timeline": True,
            }
            cursor = args.get("cursor", "")
            if cursor:
                variables["cursor"] = cursor

            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.LIKES,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            print(json.dumps({
                "status": response.status_code,
                "body": data if response.status_code == 200 else response.text
            }))

        elif action == "bookmarks":
            variables = {
                "count": args.get("count", 20),
                "includePromotedContent": True,
            }
            cursor = args.get("cursor", "")
            if cursor:
                variables["cursor"] = cursor

            bk_features = dict(FEATURES)
            bk_features["graphql_timeline_v2_bookmark_timeline"] = True

            params = flatten_params({
                'variables': variables,
                'features': bk_features,
            })
            data, response = await client.request(
                'GET', Endpoint.BOOKMARKS,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            print(json.dumps({
                "status": response.status_code,
                "body": data if response.status_code == 200 else response.text
            }))

        elif action == "followers" or action == "following":
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 20),
                "includePromotedContent": False,
            }
            cursor = args.get("cursor", "")
            if cursor:
                variables["cursor"] = cursor

            endpoint = Endpoint.FOLLOWERS if action == "followers" else Endpoint.FOLLOWING
            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', endpoint,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            print(json.dumps({
                "status": response.status_code,
                "body": data if response.status_code == 200 else response.text
            }))

        else:
            print(json.dumps({"error": f"Unknown action: {action}"}))
            sys.exit(1)

    except Exception as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
