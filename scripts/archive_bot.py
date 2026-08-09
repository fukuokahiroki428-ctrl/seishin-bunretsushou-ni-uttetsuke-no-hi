#!/usr/bin/env python3
"""
디스코드 봇 — 수집해 둔 자료에 대해 디스코드에서 묻고 답받는다.

쓰는 법:
    디스코드에서  @봇 vivoさん 그림 뭐 있어?
    또는          !ask vivoさん 그림 뭐 있어?
    검색만        !find 先生虐待
    상태          !status

토큰:
    ★ 봇 토큰은 이 파일에 적지 않는다. 환경변수로 넘긴다.
        export DISCORD_BOT_TOKEN='...'
        python3 archive_bot.py
      토큰이 코드나 저장소에 들어가면 그대로 유출된다.

사전 준비:
    1) archive_index.py 로 색인을 만든다
    2) 앱에서 로컬 AI 를 켠다 (127.0.0.1:8737)
    3) 디스코드 개발자 포털에서 봇을 만들고 Message Content Intent 를 켠다

한계:
    그림 '내용' 은 못 본다 — 번들 모델이 글자 전용이라 EXIF·파일명·엑셀 등
    글자로 남은 것만 근거가 된다.
"""
import asyncio, os, sqlite3, sys, textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import archive_ask as A            # 검색·질의 로직을 그대로 재사용한다

try:
    import discord
except ImportError:
    print("discord.py 가 없습니다.  python3 -m pip install discord.py")
    sys.exit(1)

TOKEN = os.environ.get('DISCORD_BOT_TOKEN', '').strip()
DB    = Path(os.environ.get('ARCHIVE_DB', str(A.DEFAULT_DB)))
LIMIT = 1900                        # 디스코드 한 메시지 상한(2000)보다 여유 있게


def chunks(s: str, n: int = LIMIT):
    """긴 답을 디스코드 메시지 길이에 맞춰 자른다 — 줄 단위로 끊어 읽기 좋게."""
    out, cur = [], ''
    for line in s.splitlines(keepends=True):
        if len(cur) + len(line) > n:
            out.append(cur); cur = ''
        cur += line
    if cur:
        out.append(cur)
    return out or ['(빈 응답)']


def open_db() -> sqlite3.Connection | None:
    if not DB.exists():
        return None
    # 봇은 여러 요청을 넘나들며 쓰므로 스레드 확인을 끈다(읽기 전용 사용).
    return sqlite3.connect(str(DB), check_same_thread=False)


class Bot(discord.Client):
    async def on_ready(self):
        print(f"접속: {self.user}  ·  색인 {DB}")

    async def on_message(self, msg: discord.Message):
        if msg.author.bot:
            return
        text = msg.content.strip()
        mentioned = self.user in msg.mentions
        if mentioned:
            text = text.replace(f'<@{self.user.id}>', '').replace(f'<@!{self.user.id}>', '').strip()
            cmd, arg = 'ask', text
        elif text.startswith('!ask '):
            cmd, arg = 'ask', text[5:].strip()
        elif text.startswith('!find '):
            cmd, arg = 'find', text[6:].strip()
        elif text.strip() == '!status':
            cmd, arg = 'status', ''
        else:
            return

        db = open_db()
        if db is None:
            await msg.reply(f"색인이 없습니다: `{DB}`\n먼저 `archive_index.py` 를 돌려 주세요.")
            return

        try:
            if cmd == 'status':
                n = db.execute("SELECT COUNT(*) FROM files").fetchone()[0]
                kinds = db.execute(
                    "SELECT kind,COUNT(*) FROM files GROUP BY kind ORDER BY 2 DESC").fetchall()
                meta = db.execute(
                    "SELECT COUNT(*) FROM files WHERE artist<>'' OR title<>''").fetchone()[0]
                ai = "켜짐" if A.llm_alive() else "꺼짐"
                body = [f"**색인** {n:,}건 · 작가/제목 있는 것 {meta:,}건",
                        "  " + " · ".join(f"{k} {c:,}" for k, c in kinds),
                        f"**로컬 AI** {ai}"]
                await msg.reply("\n".join(body))
                return

            if not arg:
                await msg.reply("무엇을 찾을까요? 예) `!ask vivoさん 그림 뭐 있어?`")
                return

            async with msg.channel.typing():
                # 검색·AI 호출은 블로킹이라 별도 스레드로 — 봇이 멈추지 않게.
                items = await asyncio.to_thread(A.search, db, arg, A.TOP_K)
                if not items:
                    await msg.reply("색인에서 관련 자료를 찾지 못했습니다.")
                    return

                if cmd == 'find':
                    lines = []
                    for i, d in enumerate(items[:15], 1):
                        t = f"`{i}.` {d['name']}"
                        if d['artist'] or d['title']:
                            t += f"\n     {d['artist']} — {d['title'][:60]}"
                        if d['source_url']:
                            t += f"\n     <{d['source_url']}>"
                        lines.append(t)
                    for c in chunks("\n".join(lines)):
                        await msg.reply(c) if c is lines else await msg.channel.send(c)
                    return

                if not A.llm_alive():
                    head = "로컬 AI 가 꺼져 있습니다. 앱에서 **AI 켜기** 를 눌러 주세요.\n검색 결과만 보여드립니다:\n"
                    lines = [f"`{i}.` {d['name']} — {d['artist']}" for i, d in enumerate(items[:12], 1)]
                    await msg.reply(head + "\n".join(lines))
                    return

                answer = await asyncio.to_thread(A.ask_llm, arg, A.make_context(items))
                src = "\n".join(
                    f"`[{i}]` {d['name']}" + (f" <{d['source_url']}>" if d['source_url'] else "")
                    for i, d in enumerate(items[:8], 1))
                full = f"{answer}\n\n**근거**\n{src}"
                parts = chunks(full)
                await msg.reply(parts[0])
                for p in parts[1:]:
                    await msg.channel.send(p)

        except Exception as e:
            await msg.reply(f"오류: `{type(e).__name__}: {e}`")
        finally:
            db.close()


def main() -> int:
    if not TOKEN:
        print(textwrap.dedent("""
            봇 토큰이 없습니다.

              export DISCORD_BOT_TOKEN='당신의_봇_토큰'
              python3 archive_bot.py

            토큰은 디스코드 개발자 포털 → 해당 앱 → Bot → Reset Token 에서 얻습니다.
            같은 화면에서 'MESSAGE CONTENT INTENT' 를 켜야 봇이 메시지를 읽을 수 있습니다.
            ★ 토큰은 비밀번호와 같습니다. 파일이나 저장소에 적지 마세요.
        """).strip())
        return 1
    if not DB.exists():
        print(f"색인이 없습니다: {DB}\n먼저 archive_index.py 를 돌려 주세요.")
        return 1

    intents = discord.Intents.default()
    intents.message_content = True     # 개발자 포털에서도 켜야 한다
    Bot(intents=intents).run(TOKEN)
    return 0


if __name__ == '__main__':
    sys.exit(main())
