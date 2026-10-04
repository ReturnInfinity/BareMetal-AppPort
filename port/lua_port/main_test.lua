-- main_test.lua -- smoke test for lua.app, install-main.sh's default
-- program. Exercises the core language plus every standard library
-- against this port's real backends: io/os against ext4_shim.c's
-- EXT2 file I/O and posix_shim.c's clocks, coroutines, string
-- patterns, utf8, math.random's seeding, and the error paths that
-- have to fail cleanly here (io.popen/os.execute -- no processes, see
-- OPENISSUES.md). Prints one "ok"/"FAIL" line per check and a summary.

local passed, failed = 0, 0
local function check(name, cond, detail)
	if cond then
		passed = passed + 1
		print("ok   " .. name)
	else
		failed = failed + 1
		print("FAIL " .. name .. (detail and (": " .. tostring(detail)) or ""))
	end
end

print(_VERSION .. " on BareMetal")
print("arg[0] = " .. tostring(arg and arg[0]) .. ", #arg = " .. #arg)

-- Core language
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
check("recursion", fib(20) == 6765)
check("integer/float", 7 // 2 == 3 and 7 / 2 == 3.5 and math.type(1) == "integer")
check("closures", (function() local n = 0; return function() n = n + 1; return n end end)()() == 1)
local mt = setmetatable({}, { __index = function(_, k) return k * 2 end })
check("metatables", mt[21] == 42)
check("pcall/error", select(2, pcall(error, "boom", 0)) == "boom")
check("goto", (function() local i = 0 ::top:: i = i + 1 if i < 5 then goto top end return i end)() == 5)

-- Deep C-stack recursion (Lua -> C -> Lua via pcall), the reason
-- lua.c runs on its own 512 KiB stack instead of the kernel's 64 KiB one.
local function nest(n) if n == 0 then return 0 end return select(2, pcall(nest, n - 1)) + 1 end
check("nested pcall x180", nest(180) == 180)
-- ...and the worst case measured for that stack's sizing: a gsub
-- callback chain must stop at Lua's own "C stack overflow" error, not
-- overrun the stack.
local function viac() return string.gsub("x", "x", viac) end
local ok_c, err_c = pcall(viac)
check("C stack overflow is an error", not ok_c and tostring(err_c):find("C stack overflow") ~= nil, err_c)

-- string / table / utf8
check("string.format", string.format("%5.2f|%d|%s", math.pi, 42, "x") == " 3.14|42|x")
check("patterns", ("key=value"):match("(%w+)=(%w+)") == "key")
check("gsub", ("hello world"):gsub("o", "0") == "hell0 w0rld")
check("string.pack", string.unpack("<i4", string.pack("<i4", -2)) == -2)
local t = { 5, 3, 9, 1 }
table.sort(t)
check("table.sort", table.concat(t, ",") == "1,3,5,9")
check("utf8", utf8.len("héllo") == 5 and utf8.char(0x20AC) == "€")

-- Coroutines
local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1); return b * 2 end)
check("coroutines", co(1) == 2 and co(10) == 20)

-- math
math.randomseed(42)
local r1 = math.random(1, 1000000)
math.randomseed(42)
check("math.random reseed", math.random(1, 1000000) == r1)
check("math", math.floor(math.sqrt(144)) == 12 and math.maxinteger > 0)

-- os: clocks/time (posix_shim.c's clock_gettime)
local now = os.time()
check("os.time", type(now) == "number" and now > 1700000000, now)
check("os.date", os.date("!%Y", 0) == "1970")
local c0 = os.clock()
local x = 0
for i = 1, 2000000 do x = x + i end
check("os.clock advances", os.clock() >= c0, os.clock() - c0)

-- io: real EXT2 file I/O (ext4_shim.c)
local path = "/lua/_smoke.txt"
local f, err = io.open(path, "w")
check("io.open w", f ~= nil, err)
if f then
	f:write("line one\n", 2, "\n")
	f:close()
	local lines = {}
	for l in io.lines(path) do lines[#lines + 1] = l end
	check("io.lines", #lines == 2 and lines[1] == "line one" and lines[2] == "2")
	local rf = io.open(path, "r")
	rf:seek("set", 5)
	check("file:seek", rf:read(3) == "one")
	rf:close()
	check("os.rename", os.rename(path, path .. ".2") == true)
	check("renamed", io.open(path, "r") == nil and io.open(path .. ".2", "r") ~= nil)
	-- rename() over an existing file replaces it (POSIX semantics,
	-- ext4_shim_rename())
	local o = io.open(path, "w"); o:write("other"); o:close()
	check("os.rename replaces", os.rename(path, path .. ".2") == true and io.open(path .. ".2"):read("a") == "other")
	check("os.remove", os.remove(path .. ".2") == true)
	check("removed", io.open(path .. ".2", "r") == nil)
	check("os.remove missing fails", os.remove(path .. ".2") == nil)
end

-- require() from /lua (LUAMAIN_PACKAGE_PATH in lua.c)
local mf = io.open("/lua/_smokemod.lua", "w")
if mf then
	mf:write("return { answer = 42 }\n")
	mf:close()
	local ok, mod = pcall(require, "_smokemod")
	check("require from /lua", ok and mod.answer == 42, mod)
	os.remove("/lua/_smokemod.lua")
end

-- No processes on this port: these must fail cleanly, not hang/crash.
local ok_popen = pcall(io.popen, "ls")
local p = ok_popen and io.popen("ls") or nil
check("io.popen fails cleanly", p == nil)
check("os.execute fails cleanly", os.execute("true") ~= true)

-- Garbage collector. Sized to fit the 2 MiB boot RAM window with no
-- hot-plug budget at all, so this passes on any VM.
local before = collectgarbage("count")
do local junk = {} for i = 1, 5000 do junk[i] = { i } end end
collectgarbage()
check("gc", collectgarbage("count") < before + 1024)

print(string.format("%d passed, %d failed", passed, failed))
