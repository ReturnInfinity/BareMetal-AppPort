-- files.lua -- word-frequency count over a file written to and read back
-- from the EXT2 disk image (io/os on top of ext4_shim.c).

local path = "/lua/words.txt"

local out = assert(io.open(path, "w"))
out:write([[
the quick brown fox jumps over the lazy dog
the dog barks and the fox runs
]])
out:close()

local counts = {}
for line in io.lines(path) do
	for word in line:gmatch("%a+") do
		counts[word] = (counts[word] or 0) + 1
	end
end

local words = {}
for w in pairs(counts) do words[#words + 1] = w end
table.sort(words, function(a, b)
	if counts[a] ~= counts[b] then return counts[a] > counts[b] end
	return a < b
end)

for i = 1, math.min(5, #words) do
	print(string.format("%-6s %d", words[i], counts[words[i]]))
end

assert(os.remove(path))
print(string.format("done at %s", os.date("!%Y-%m-%d %H:%M:%S UTC")))
