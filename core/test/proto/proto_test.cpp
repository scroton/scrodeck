// core/test/proto/proto_test.cpp — the frame codec against its vectors and its own round trips.
//   powershell -File core/test/proto/build.ps1
#include "../../proto/frames.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace deck::proto;
static int pass = 0, fail = 0;
static void check(bool ok, const std::string& name) { if (ok) pass++; else { fail++; std::printf("FAIL  %s\n", name.c_str()); } }

int main(int argc, char** argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "core/test/fixtures";
  std::ifstream f(fixtures + "/proto-synth.json", std::ios::binary);
  if (!f) { std::printf("cannot open %s/proto-synth.json\n", fixtures.c_str()); return 2; }
  std::stringstream ss; ss << f.rdbuf();
  json vec = json::parse(ss.str());
  int n = 0;
  for (const auto& v : vec) {
    std::string d; for (const auto& b : v["d"]) d.push_back((char)b.get<int>());
    check(synthesised(d) == v["want"].get<bool>(), "synthesised #" + std::to_string(n));
    n++;
  }
  // shell -> core: the exact shapes lib/phone.js onMessage reads
  check(encode(call(7, "deskInit")) == R"({"args":[],"fn":"deskInit","rid":7,"t":"call"})", "call deskInit");
  check(encode(call(0, "userInput", json::array({ "t1" }))) == R"({"args":["t1"],"fn":"userInput","rid":0,"t":"call"})", "fire userInput");
  check(encode(attach("a")) == R"({"id":"a","since":false,"t":"attach"})", "attach");
  check(encode(attach("a", true)) == R"({"id":"a","since":true,"t":"attach"})", "attach since");
  check(encode(detach("a")) == R"({"id":"a","t":"detach"})", "detach");
  check(encode(detachAll()) == R"({"t":"detach"})", "detach all");
  check(encode(input("a", "x\r")) == R"({"d":"x\r","id":"a","kind":"key","t":"input"})", "input is kind key");
  // the wheel (R8 narrowed, 2026-11-18): kind 'wheel', and only ever one SGR wheel report
  check(encode(wheel("a", "\x1b[<64;10;5M")) == R"({"d":"\u001b[<64;10;5M","id":"a","kind":"wheel","t":"input"})", "wheel is kind wheel");
  check(!wheel("a", "\x1b[<65;1;1M").is_null() && !wheel("a", "\x1b[<68;3;4M").is_null() && !wheel("a", "\x1b[<81;3;4M").is_null(), "wheel: down, and the modifier bits");
  check(wheel("a", "\x1b[<0;10;5M").is_null() && wheel("a", "\x1b[<64;10;5m").is_null() && wheel("a", "\x1b[<32;10;5M").is_null() && wheel("a", "\x1b[<66;1;1M").is_null(), "wheel: never a click, a release, motion or another button");
  // a click (R8 narrowed, 2026-11-19): kind 'click', only ever one SGR LEFT press or release
  check(encode(click("a", "\x1b[<0;10;5M")) == R"({"d":"\u001b[<0;10;5M","id":"a","kind":"click","t":"input"})", "click is kind click");
  check(!click("a", "\x1b[<0;10;5m").is_null() && !click("a", "\x1b[<16;3;4M").is_null() && !click("a", "\x1b[<8;3;4m").is_null(), "click: the release, and the modifier bits");
  check(click("a", "\x1b[<1;10;5M").is_null() && click("a", "\x1b[<2;10;5M").is_null() && click("a", "\x1b[<32;10;5M").is_null() && click("a", "\x1b[<35;10;5M").is_null() && click("a", "\x1b[<64;10;5M").is_null() && click("a", "\x1b[<3;10;5M").is_null(), "click: never the middle or right button, a drag, motion or the wheel");
  check(click("a", "\x1b[<0;10;5M\x1b[<0;10;5m").is_null() && click("a", "\x1b[M !!").is_null() && click("a", "\x1b[I").is_null() && click("a", "x").is_null() && click("a", "").is_null() && click("a", "\x1b[<0;10;5Mx").is_null() && click("a", "\x1b[<0;0;5M").is_null() && click("a", "\x1b[<0;-1;5M").is_null(), "click: never two reports, X10, a focus report, text, cell 0");
  check(wheel("a", "\x1b[<64;10;5M\x1b[<64;10;5M").is_null() && wheel("a", "\x1b[M`!!").is_null() && wheel("a", "\x1b[I").is_null() && wheel("a", "x").is_null() && wheel("a", "").is_null() && wheel("a", "\x1b[<64;10;5Mx").is_null() && wheel("a", "\x1b[< 64;10;5M").is_null() && wheel("a", "\x1b[<64;-1;5M").is_null(), "wheel: never two reports, X10, a focus report, text");
  check(encode(call(7, "deskInit", json::array({ json{ { "name", "LAPTOP" } } }))) == R"({"args":[{"name":"LAPTOP"}],"fn":"deskInit","rid":7,"t":"call"})", "call deskInit with a name");
  check(encode(resize("a", 120, 40)) == R"({"cols":120,"id":"a","rows":40,"t":"resize"})", "resize");
  check(encode(shown({ "a", "b" }, false)) == R"({"hidden":false,"ids":["a","b"],"t":"shown"})", "shown");
  check(encode(layout({ { "a", { 1, 2 } }, { "b", { 3 } } })) == R"({"layout":{"a":[1,2],"b":[3]},"t":"layout"})", "layout");
  check(encode(leaving("a", true)) == R"({"id":"a","on":true,"t":"leaving"})", "leaving");
  // a pty can cut a UTF-8 character in half: the frame still encodes
  check(encode(input("a", std::string("\xE2\x82", 2))).find("\"t\":\"input\"") != std::string::npos, "input with a split character encodes");
  // core -> shell
  Frame d = parse(R"({"t":"data","id":"a","d":"hi","replay":true})");
  check(d.kind == Kind::Data && d.id() == "a" && d.data() == "hi" && d.replay(), "parse data");
  Frame ok = parse(R"({"t":"ok","rid":3,"result":{"windowIndex":2}})");
  check(ok.kind == Kind::Ok && ok.rid() == 3 && ok.result().value("windowIndex", 0) == 2, "parse ok");
  Frame err = parse(R"({"t":"err","rid":4,"error":"one desk at a time: x"})");
  check(err.kind == Kind::Err && err.rid() == 4 && err.error().find("one desk at a time") == 0, "parse err");
  Frame ev = parse(R"({"t":"event","name":"layout:evict","payload":["a"]})");
  check(ev.kind == Kind::Event && ev.name() == "layout:evict" && ev.payload().is_array(), "parse event");
  Frame ex = parse(R"({"t":"exit","id":"a","exitCode":3})");
  check(ex.kind == Kind::Exit && ex.exitCode() == 3, "parse exit");
  check(parse(R"({"t":"reset","id":"a"})").kind == Kind::Reset, "parse reset");
  check(parse(R"({"t":"meta","meta":{"tiles":[]}})").meta().contains("tiles"), "parse meta");
  check(parse(R"({"t":"init","meta":{}})").kind == Kind::Init, "parse init");
  check(parse("not json").kind == Kind::Unknown && parse("[1]").kind == Kind::Unknown && parse(R"({"t":"nope"})").kind == Kind::Unknown, "junk is Unknown, never a throw");
  std::printf("%d/%d passed\n", pass, pass + fail);
  return fail ? 1 : 0;
}
