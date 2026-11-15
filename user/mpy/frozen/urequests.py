# MIT License - LugalOS MicroPython urequests (backed by kernel net/http + net/tls)
import json
import lugal


class Response:
    def __init__(self, status_code, content):
        self.status_code = status_code
        self.content = content

    @property
    def text(self):
        return self.content.decode("utf-8")

    def json(self):
        return json.loads(self.content.decode("utf-8"))

    def close(self):
        self.content = b""


def request(method, url, **kwargs):
    status, body = lugal.http_request(method, url)
    return Response(status, body)


def get(url, **kwargs):
    return request("GET", url, **kwargs)


def post(url, **kwargs):
    return request("POST", url, **kwargs)


def head(url, **kwargs):
    return request("HEAD", url, **kwargs)
