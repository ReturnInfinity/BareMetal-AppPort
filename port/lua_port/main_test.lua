-- main_test.lua -- a smoke test for this port, meant to be deployed
-- as /lualib/main.lua via install-main.sh. Not part of upstream Lua --
-- this port's own file.
--
-- Each section is independent and self-reporting (PASS/FAIL, not an
-- assert that aborts the rest) -- one broken section shouldn't hide
-- whether everything else still works. Covers core language, the
-- string/table/math/os libraries, coroutines, and real file I/O
-- against the EXT2 disk image itself (see port/ext4_shim.c).

local results = {}

local function check(name, fn)
	local ok, err = pcall(fn)
	table.insert(results, {name = name, ok = ok, err = err})
	if ok then
		print("PASS: " .. name)
	else
		print("FAIL: " .. name .. " -> " .. tostring(err))
	end
end

check("core_language", function()
	assert(1 + 1 == 2)
	local squares = {}
	for i = 0, 4 do squares[#squares + 1] = i * i end
	assert(squares[1] == 0 and squares[5] == 16)

	local t = {a = 1, b = 2}
	assert(t.b == 2)

	local Point = {}
	Point.__index = Point
	function Point.new(x, y)
		return setmetatable({x = x, y = y}, Point)
	end
	function Point:__tostring()
		return "Point(" .. self.x .. ", " .. self.y .. ")"
	end
	local p = Point.new(3, 4)
	assert(tostring(p) == "Point(3, 4)")
end)

check("string_library", function()
	assert(string.format("hello %s", "world") == "hello world")
	assert(string.upper("lua") == "LUA")
	assert(string.rep("ab", 3) == "ababab")
	assert(string.match("key=value", "(%w+)=(%w+)") == "key")
	assert(("  trim me  "):gsub("^%s+", ""):gsub("%s+$", "") == "trim me")
end)

check("table_library", function()
	local t = {5, 3, 1, 4, 2}
	table.sort(t)
	assert(table.concat(t, ",") == "1,2,3,4,5")
	table.insert(t, 6)
	assert(#t == 6)
	table.remove(t, 1)
	assert(t[1] == 2)
end)

check("math_library", function()
	assert(math.max(1, 5, 3) == 5)
	assert(math.floor(3.7) == 3)
	assert(math.abs(-2) == 2)
	local n = tostring(0)
	assert(n == "0")
end)

check("coroutines", function()
	local co = coroutine.create(function(a)
		local b = coroutine.yield(a + 1)
		return b + 1
	end)
	local ok1, v1 = coroutine.resume(co, 10)
	assert(ok1 and v1 == 11)
	local ok2, v2 = coroutine.resume(co, 20)
	assert(ok2 and v2 == 21)
	assert(coroutine.status(co) == "dead")
end)

check("file_io", function()
	local path = "/lualib_test_tmp.txt"
	local f = assert(io.open(path, "w"))
	f:write("hello from lua\n")
	f:close()

	local rf = assert(io.open(path, "r"))
	local content = rf:read("l")
	rf:close()
	assert(content == "hello from lua")

	os.remove(path)
end)

check("pcall_error_handling", function()
	local ok, err = pcall(function() error("boom") end)
	assert(not ok)
	assert(tostring(err):find("boom"))
end)

print("")
print(_VERSION .. " smoke test on BareMetal-AppPort")

local failed = 0
for _, r in ipairs(results) do
	if not r.ok then failed = failed + 1 end
end
print(string.format("%d/%d checks passed", #results - failed, #results))
