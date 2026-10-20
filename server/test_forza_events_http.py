"""Real HTTP checks for the organizer boundary and validated mutation API."""
import asyncio
import json
import tempfile
import os
import unittest
from unittest.mock import patch
import forza_events as ev
import forza_live_server as srv
import forza_live_lsp as lsp


class EventsHTTPTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.server = srv.ForzaLive("test", 16)
        self.server.events.db.close()
        self.server.events = ev.Events(self.server, os.path.join(self.temp.name,"events.sqlite3"))
        self.http = await asyncio.start_server(lambda r,w: srv.http_handler(self.server,r,w), "127.0.0.1",0)
        self.port = self.http.sockets[0].getsockname()[1]
        self.token = patch.object(lsp,"ADMIN_TOKEN","event-admin-test");self.token.start()

    async def asyncTearDown(self):
        self.http.close();await self.http.wait_closed()
        self.server.events.db.close();self.token.stop();self.temp.cleanup()

    async def request(self, method="GET", path="/admin/events", data=None, authorized=True, origin=None, content_type="application/json"):
        reader, writer = await asyncio.open_connection("127.0.0.1",self.port)
        body = b"" if data is None else json.dumps(data).encode()
        headers = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close",
                   "X-Forwarded-For: 203.0.113.10",f"Content-Type: {content_type}",f"Content-Length: {len(body)}"]
        if authorized:headers.append("X-Admin-Token: event-admin-test")
        if origin:headers.append("Origin: "+origin)
        writer.write(("\r\n".join(headers)+"\r\n\r\n").encode()+body);await writer.drain()
        raw=await reader.read();writer.close();await writer.wait_closed()
        head,body=raw.split(b"\r\n\r\n",1)
        return int(head.split()[1]),body

    async def test_unauthorized_proxy_cannot_list_or_create_events(self):
        self.assertEqual((await self.request(authorized=False))[0],403)
        self.assertEqual((await self.request("POST",data={},authorized=False))[0],403)
        self.assertEqual(self.server.events.list(),[])

    async def test_cross_origin_and_simple_form_mutations_refused(self):
        self.assertEqual((await self.request("POST",data={},origin="https://untrusted.example"))[0],400)
        self.assertEqual((await self.request("POST",data={},content_type="text/plain"))[0],400)
        self.assertEqual(self.server.events.list(),[])

    async def test_validated_create_list_start_and_invalid_methods(self):
        import time
        now=time.time()
        data=dict(id="http-event",name="Cruise",description="Local test",activity="Free roam",
                  session="0123456789abcdef",starts=now-1,ends=now+300,rewards=[1197],organizer="Test organizer")
        self.assertEqual((await self.request("POST",data=data))[0],200)
        self.assertEqual(len(json.loads((await self.request())[1])["events"]),1)
        self.assertEqual((await self.request("POST","/admin/events/http-event/start",{}))[0],200)
        self.assertEqual((await self.request("GET","/admin/events/http-event/award"))[0],400)
        self.assertEqual((await self.request("POST",data=[]))[0],400)
        self.assertEqual((await self.request("POST",data=data))[0],400)


if __name__ == "__main__":unittest.main()
