# main_test.py -- EXPERIMENTAL (see ../../PYTHON_PORT.md). A smoke test
# for this port, meant to be deployed as /pylib/main.py via
# install-main.sh. Not part of CPython -- this port's own file.
#
# Each section is independent and self-reporting (PASS/FAIL, not an
# assert that aborts the rest) -- one broken section shouldn't hide
# whether everything else still works. Sticks to what Phases 1-3
# actually proved works on this port (see PYTHON_PORT.md): core
# language, the frozen bootstrap modules, _socket, and the /pylib
# stdlib slice install-stdlib-phase3.sh installs (json, re, collections,
# encodings.ascii, ...). The _thread section is the one genuinely new
# thing being tested here -- Modules/_threadmodule.c has been compiled
# and linked in since Phase 1 (config_baremetal.c) but never actually
# exercised from Python before this file, so it's wrapped in a timeout
# rather than a plain blocking acquire() in case thread_shim.c's
# cooperative scheduler doesn't do what CPython's own thread_pthread.h
# assumes -- see that section's own comment.

results = []


def check(name, fn):
	try:
		fn()
		results.append((name, True, None))
		print("PASS:", name)
	except Exception as e:
		results.append((name, False, repr(e)))
		print("FAIL:", name, "->", repr(e))


def test_core_language():
	assert 1 + 1 == 2
	assert [x * x for x in range(5)] == [0, 1, 4, 9, 16]
	assert {"a": 1, "b": 2}["b"] == 2
	assert "hello %s" % "world" == "hello world"
	assert sum(range(10)) == 45

	class Point:
		def __init__(self, x, y):
			self.x, self.y = x, y

		def __repr__(self):
			return "Point(%r, %r)" % (self.x, self.y)

	p = Point(1, 2)
	assert repr(p) == "Point(1, 2)"

	try:
		1 / 0
	except ZeroDivisionError:
		pass
	else:
		raise AssertionError("expected ZeroDivisionError")


def test_sys_module():
	import sys
	assert sys.version_info[0] == 3
	assert isinstance(sys.path, list)
	print("  sys.version:", sys.version.split()[0])
	print("  sys.path:", sys.path)


def test_os_module():
	# os is one of Phase 1's frozen bootstrap modules, backed by real
	# ext4_shim.c syscalls (open/read/write/stat/unlink), not stubs.
	import os
	cwd = os.getcwd()
	print("  os.getcwd():", cwd)

	path = "/pylib/main_test_scratch.txt"
	with open(path, "w") as f:
		f.write("hello from main_test.py\n")
	with open(path, "r") as f:
		content = f.read()
	assert content == "hello from main_test.py\n"
	st = os.stat(path)
	assert st.st_size == len(content)
	os.unlink(path)
	try:
		os.stat(path)
	except FileNotFoundError:
		pass
	else:
		raise AssertionError("expected FileNotFoundError after unlink")


def test_time_module():
	import time
	t0 = time.monotonic()
	t1 = time.monotonic()
	assert t1 >= t0
	print("  time.time():", time.time())


def test_json_module():
	# Real file on /pylib (install-stdlib-phase3.sh), not frozen --
	# proves Phase 3's filesystem-import path, not just that json
	# exists.
	import json
	data = {"a": [1, 2, 3], "b": None, "c": True, "d": "text"}
	encoded = json.dumps(data)
	decoded = json.loads(encoded)
	assert decoded == data


def test_re_module():
	import re
	m = re.match(r"(\w+)@(\w+)\.com", "user@example.com")
	assert m is not None
	assert m.group(1) == "user"
	assert m.group(2) == "example"


def test_collections_module():
	import collections
	Point = collections.namedtuple("Point", ["x", "y"])
	p = Point(3, 4)
	assert p.x == 3 and p.y == 4
	d = collections.OrderedDict([("a", 1), ("b", 2)])
	assert list(d.keys()) == ["a", "b"]


def test_encodings_ascii():
	# Not frozen -- only reachable via pymain_baremetal.c's
	# encodings.__path__.append('/pylib/encodings') fix (Phase 3).
	import encodings.ascii
	assert encodings.ascii.getregentry().name == "ascii"
	assert "hello".encode("ascii") == b"hello"


def test_socket_module():
	# _socket is a static built-in C module (Phase 2, config_baremetal.c)
	# -- not the pure-Python socket.py wrapper, which isn't installed.
	import _socket
	s = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
	try:
		s.bind(("0.0.0.0", 0))
	finally:
		s.close()


def test_thread_module():
	# EXPERIMENTAL -- see this file's header. thread_shim.c's cooperative
	# pthreads have been exercised from C (threads.c) but never from
	# Python's _thread module before this. A lock with a bounded
	# acquire() timeout is used instead of a plain blocking acquire()
	# so a real scheduling problem here reports FAIL rather than
	# hanging this whole script (and the VM's console output) forever.
	import _thread

	done = _thread.allocate_lock()
	done.acquire()
	result = []

	def worker():
		result.append(6 * 7)
		done.release()

	_thread.start_new_thread(worker, ())

	if not done.acquire(True, 5.0):  # 5s timeout
		raise AssertionError("worker thread did not complete within 5s")
	assert result == [42]


check("core language", test_core_language)
check("sys module", test_sys_module)
check("os module (real EXT2 file I/O)", test_os_module)
check("time module", test_time_module)
check("json module (real /pylib file)", test_json_module)
check("re module (real /pylib file)", test_re_module)
check("collections module (real /pylib file)", test_collections_module)
check("encodings.ascii (real /pylib file, frozen-package __path__ fix)", test_encodings_ascii)
check("_socket module (socket/bind/close)", test_socket_module)
check("_thread module (EXPERIMENTAL)", test_thread_module)

passed = sum(1 for _, ok, _ in results if ok)
print("")
print("%d/%d checks passed" % (passed, len(results)))
if passed != len(results):
	print("FAILED:", [name for name, ok, _ in results if not ok])
