#!/usr/bin/env python3
"""
자료 질의응답 — 기존 디스코드 봇에 끼워 쓰는 부품(cog).

이미 돌리는 봇이 있을 때, 봇을 하나 더 띄우지 않고 이 파일만 얹으면 된다.
토큰은 기존 봇 것을 그대로 쓰므로 새로 발급할 것도, 배포물에 넣을 것도 없다.

붙이는 법 — 기존 봇 코드에 두 줄:

    # discord.py 2.x (commands.Bot)
    from archive_cog import setup_archive
    await setup_archive(bot)              # setup_hook 안이나 on_ready 전에

    # 또는 확장으로
    await bot.load_extension('archive_cog')

명령:
    !ask <질문>     자료를 찾아 AI 가 답한다 (근거 링크 포함)
    !find <검색어>  AI 없이 검색 결과만
    !archive        색인·AI 상태

★ 접근 제한
    기본은 '아무도 못 씀' 이다. 환경변수로 허용 계정을 지정해야 동작한다.

        export ARCHIVE_ALLOW_USERS='123456789012345678'      # 쉼표로 여러 명
        export ARCHIVE_ALLOW_CHANNELS='987654321098765432'   # (선택) 채널도 제한

    수집한 자료는 개인 기록이다. 제한을 안 걸면 봇이 있는 서버의 누구나
    보관함 전체를 조회할 수 있게 되므로, 기본값을 '차단' 으로 두었다.
    (디스코드에서 개발자 모드를 켜면 계정 우클릭 → 'ID 복사' 로 숫자를 얻는다)
"""
import asyncio, os, sqlite3, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import archive_ask as A

from discord.ext import commands

DB = Path(os.environ.get('ARCHIVE_DB', str(A.DEFAULT_DB)))
MSG_LIMIT = 1900


def _ids(env: str) -> set[int]:
    out = set()
    for tok in os.environ.get(env, '').replace(' ', '').split(','):
        if tok.isdigit():
            out.add(int(tok))
    return out


ALLOW_USERS    = _ids('ARCHIVE_ALLOW_USERS')
ALLOW_CHANNELS = _ids('ARCHIVE_ALLOW_CHANNELS')


def chunks(s: str, n: int = MSG_LIMIT) -> list[str]:
    out, cur = [], ''
    for line in s.splitlines(keepends=True):
        if len(cur) + len(line) > n:
            out.append(cur); cur = ''
        cur += line
    if cur:
        out.append(cur)
    return out or ['(빈 응답)']


class Archive(commands.Cog):
    """수집 자료 검색·질의응답."""

    def __init__(self, bot):
        self.bot = bot

    # ── 접근 제한 ──────────────────────────────────────────────────────────
    async def cog_check(self, ctx) -> bool:
        """
        모든 명령에 공통 적용. 허용 목록이 비어 있으면 아무도 통과 못 한다
        (설정을 깜빡했을 때 자료가 열리는 쪽이 아니라 닫히는 쪽으로 실패하게).
        """
        if not ALLOW_USERS:
            await ctx.reply(
                "접근 허용 계정이 지정되지 않아 잠겨 있습니다.\n"
                "봇을 켜는 쪽에서 `ARCHIVE_ALLOW_USERS` 에 디스코드 계정 ID 를 넣어 주세요.")
            return False
        if ctx.author.id not in ALLOW_USERS:
            return False                      # 조용히 무시 — 있는 줄도 모르게
        if ALLOW_CHANNELS and ctx.channel.id not in ALLOW_CHANNELS:
            return False
        return True

    # ── 공통 ───────────────────────────────────────────────────────────────
    def _db(self):
        if not DB.exists():
            return None
        return sqlite3.connect(str(DB), check_same_thread=False)

    async def _send(self, ctx, text: str):
        parts = chunks(text)
        await ctx.reply(parts[0])
        for p in parts[1:]:
            await ctx.send(p)

    # ── 명령 ───────────────────────────────────────────────────────────────
    @commands.command(name='ask')
    async def ask(self, ctx, *, question: str = ''):
        if not question:
            await ctx.reply("무엇을 찾을까요? 예) `!ask vivoさん 그림 뭐 있어?`"); return
        db = self._db()
        if db is None:
            await ctx.reply(f"색인이 없습니다: `{DB}`\n먼저 `archive_index.py` 를 돌려 주세요."); return
        try:
            async with ctx.typing():
                items = await asyncio.to_thread(A.search, db, question, A.TOP_K)
                if not items:
                    await ctx.reply("색인에서 관련 자료를 찾지 못했습니다."); return
                if not A.llm_alive():
                    lines = [f"`{i}.` {d['name']} — {d['artist']}" for i, d in enumerate(items[:12], 1)]
                    await self._send(ctx, "로컬 AI 가 꺼져 있습니다. 앱에서 **AI 켜기** 를 눌러 주세요.\n"
                                          "검색 결과만 보여드립니다:\n" + "\n".join(lines))
                    return
                answer = await asyncio.to_thread(A.ask_llm, question, A.make_context(items))
                src = "\n".join(
                    f"`[{i}]` {d['name']}" + (f" <{d['source_url']}>" if d['source_url'] else "")
                    for i, d in enumerate(items[:8], 1))
                await self._send(ctx, f"{answer}\n\n**근거**\n{src}")
        except Exception as e:
            await ctx.reply(f"오류: `{type(e).__name__}: {e}`")
        finally:
            db.close()

    @commands.command(name='find')
    async def find(self, ctx, *, query: str = ''):
        if not query:
            await ctx.reply("검색어를 적어 주세요. 예) `!find 先生虐待`"); return
        db = self._db()
        if db is None:
            await ctx.reply(f"색인이 없습니다: `{DB}`"); return
        try:
            items = await asyncio.to_thread(A.search, db, query, 15)
            if not items:
                await ctx.reply("찾지 못했습니다."); return
            lines = []
            for i, d in enumerate(items, 1):
                t = f"`{i}.` {d['name']}"
                if d['artist'] or d['title']:
                    t += f"\n     {d['artist']} — {d['title'][:60]}"
                if d['source_url']:
                    t += f"\n     <{d['source_url']}>"
                lines.append(t)
            await self._send(ctx, "\n".join(lines))
        finally:
            db.close()

    @commands.command(name='archive')
    async def status(self, ctx):
        db = self._db()
        if db is None:
            await ctx.reply(f"색인 없음: `{DB}`"); return
        try:
            n = db.execute("SELECT COUNT(*) FROM files").fetchone()[0]
            kinds = db.execute("SELECT kind,COUNT(*) FROM files GROUP BY kind ORDER BY 2 DESC").fetchall()
            meta = db.execute("SELECT COUNT(*) FROM files WHERE artist<>'' OR title<>''").fetchone()[0]
            ai = "켜짐" if A.llm_alive() else "꺼짐"
            await ctx.reply(
                f"**색인** {n:,}건 · 작가/제목 있는 것 {meta:,}건\n"
                + "  " + " · ".join(f"{k} {c:,}" for k, c in kinds)
                + f"\n**로컬 AI** {ai}\n**허용 계정** {len(ALLOW_USERS)}명"
                + (f" · 채널 제한 {len(ALLOW_CHANNELS)}곳" if ALLOW_CHANNELS else ""))
        finally:
            db.close()


async def setup_archive(bot):
    """기존 봇에서 직접 부를 때."""
    await bot.add_cog(Archive(bot))


async def setup(bot):
    """bot.load_extension('archive_cog') 로 붙일 때."""
    await bot.add_cog(Archive(bot))
