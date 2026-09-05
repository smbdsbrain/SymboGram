#!/usr/bin/env python3
"""An independent Telegram client, used as counterparty and oracle for the e2e tier.

Why a second implementation at all: if SymboGram both sends a message and checks
that it arrived, a symmetric bug in its own TL codec passes the test. Telethon
speaks the same protocol with entirely different code, so "the oracle can see
what we sent" is evidence the bytes on the wire were right, not just
self-consistent.

Telethon is NOT ground truth. It is another MTProto implementation with its own
bugs, and it speaks a far newer layer than SymboGram does. When the two
disagree that is a question, not a verdict -- and inbound objects that layer 229
does not know are expected skew, not defects.

Credentials come from secrets/telegram.yaml, never from argv: an argument lands
in shell history and in the process list. The Telethon session is written to
secrets/, which is gitignored several times over.

Subcommands:
    seed     sign in to a test-DC account and record it in local/e2e-fixture.ini
    send     send a message to a peer (inbound traffic for the client under test)
    expect   wait for a message and assert its text -- the oracle half
    whoami   print the signed-in account, for checking the fixture is live
"""

import argparse
import asyncio
import configparser
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SECRETS = os.path.join(REPO, "secrets")
FIXTURE = os.path.join(REPO, "local", "e2e-fixture.ini")

# Public. Same addresses tdesktop compiles in, and the same DC 2 that
# libkg/tgtransport.cpp already bootstraps to in test mode.
TEST_DCS = {
    1: ("149.154.175.10", 443),
    2: ("149.154.167.40", 443),
    3: ("149.154.175.117", 443),
}


def fail(msg):
    print("FAILED: " + msg, file=sys.stderr)
    sys.exit(2)


def read_credentials():
    """api_id / api_hash from secrets/telegram.yaml, or the environment.

    Same precedence as tools/write-apisecrets.ps1: the environment wins, so a
    run can be given credentials without them touching disk.
    """
    api_id = os.environ.get("TG_API_ID")
    api_hash = os.environ.get("TG_API_HASH")
    if api_id and api_hash:
        return int(api_id), api_hash

    path = os.path.join(SECRETS, "telegram.yaml")
    if not os.path.exists(path):
        fail("no secrets/telegram.yaml and no TG_API_ID/TG_API_HASH in the environment")

    values = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or ":" not in line:
                continue
            k, v = line.split(":", 1)
            values[k.strip()] = v.strip().strip('"').strip("'")

    if "api_id" not in values or "api_hash" not in values:
        fail("secrets/telegram.yaml has no api_id/api_hash")
    return int(values["api_id"]), values["api_hash"]


def dc_of(phone):
    m = re.match(r"^99966(\d)\d{4}$", phone)
    if not m:
        fail("not a test phone number: expected 99966XYYYY")
    return int(m.group(1))


def make_client(phone, session_name):
    from telethon import TelegramClient

    api_id, api_hash = read_credentials()
    dc = dc_of(phone)
    if dc not in TEST_DCS:
        fail("test DC %d is not one of 1, 2, 3" % dc)

    # Session lives under secrets/: it is an auth key, even for a throwaway
    # account, and secrets/ is where auth keys go in this repository.
    os.makedirs(SECRETS, exist_ok=True)
    path = os.path.join(SECRETS, "e2e-oracle-%s" % session_name)

    client = TelegramClient(path, api_id, api_hash)
    # Telethon needs to be told about the test environment explicitly; it has
    # no notion of a 99966 number meaning anything.
    ip, port = TEST_DCS[dc]
    client.session.set_dc(dc, ip, port)
    return client, dc


async def do_seed(args):
    client, dc = make_client(args.phone, args.session)
    # The test environment's code is the DC id repeated. Length comes from the
    # server; five is the documented value.
    await client.connect()
    if not await client.is_user_authorized():
        sent = await client.send_code_request(args.phone)
        code = str(dc) * 5
        from telethon.errors import PhoneCodeInvalidError
        try:
            await client.sign_in(phone=args.phone, code=code,
                                 phone_code_hash=sent.phone_code_hash)
        except PhoneCodeInvalidError:
            # Six is the documented fallback. If both are rejected the fixed-code
            # rule is not in force on this DC today, which is a finding about
            # Telegram rather than about us -- say so rather than retrying.
            await client.sign_in(phone=args.phone, code=str(dc) * 6,
                                 phone_code_hash=sent.phone_code_hash)

    me = await client.get_me()
    os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
    cfg = configparser.ConfigParser()
    if os.path.exists(FIXTURE):
        cfg.read(FIXTURE)
    if "oracle" not in cfg:
        cfg["oracle"] = {}
    cfg["oracle"][args.session + "_phone"] = args.phone
    cfg["oracle"][args.session + "_id"] = str(me.id)
    cfg["oracle"][args.session + "_username"] = me.username or ""
    with open(FIXTURE, "w", encoding="utf-8") as fh:
        cfg.write(fh)

    print("seeded %s: id=%s dc=%d" % (args.session, me.id, dc))
    print("fixture: %s (gitignored)" % os.path.relpath(FIXTURE, REPO))
    await client.disconnect()
    return 0


async def do_whoami(args):
    client, dc = make_client(args.phone, args.session)
    await client.connect()
    if not await client.is_user_authorized():
        print("not authorized; run 'seed' first")
        await client.disconnect()
        return 1
    me = await client.get_me()
    print("id=%s username=%s dc=%d" % (me.id, me.username, dc))
    await client.disconnect()
    return 0


async def do_send(args):
    client, _ = make_client(args.phone, args.session)
    await client.connect()
    if not await client.is_user_authorized():
        fail("oracle is not signed in; run 'seed' first")
    target = int(args.to) if args.to.lstrip("-").isdigit() else args.to
    msg = await client.send_message(target, args.text)
    print("sent id=%s" % msg.id)
    await client.disconnect()
    return 0


async def do_expect(args):
    """Wait for a message matching --text in a peer's history.

    Polls rather than listening for updates: the client under test may send
    before the oracle connects, and a poll sees history either way. Concurrency
    in an e2e suite is where flakiness comes from, so it is avoided wherever the
    same assertion can be made after the fact.
    """
    client, _ = make_client(args.phone, args.session)
    await client.connect()
    if not await client.is_user_authorized():
        fail("oracle is not signed in; run 'seed' first")

    target = int(args.frm) if args.frm.lstrip("-").isdigit() else args.frm
    deadline = asyncio.get_event_loop().time() + args.timeout
    while asyncio.get_event_loop().time() < deadline:
        async for m in client.iter_messages(target, limit=args.limit):
            if m.message and args.text in m.message:
                print("found id=%s" % m.id)
                await client.disconnect()
                return 0
        await asyncio.sleep(2)

    print("NOT FOUND: no message containing the expected text within %ds" % args.timeout)
    await client.disconnect()
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", default="a", help="which oracle account (default: a)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("seed");   p.add_argument("--phone", required=True)
    p = sub.add_parser("whoami"); p.add_argument("--phone", required=True)
    p = sub.add_parser("send")
    p.add_argument("--phone", required=True)
    p.add_argument("--to", required=True)
    p.add_argument("--text", required=True)
    p = sub.add_parser("expect")
    p.add_argument("--phone", required=True)
    p.add_argument("--from", dest="frm", required=True)
    p.add_argument("--text", required=True)
    p.add_argument("--timeout", type=int, default=60)
    p.add_argument("--limit", type=int, default=20)

    args = ap.parse_args()
    handler = {"seed": do_seed, "whoami": do_whoami,
               "send": do_send, "expect": do_expect}[args.cmd]
    return asyncio.get_event_loop().run_until_complete(handler(args))


if __name__ == "__main__":
    sys.exit(main())
