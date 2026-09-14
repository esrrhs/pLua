-- Unit tests for pLua
local cpath_candidates = {
    "../bin/?.so",
    "../../bin/?.so",
    "bin/?.so",
    "./?.so"
}
for _, p in ipairs(cpath_candidates) do
    package.cpath = p .. ";" .. package.cpath
end

local function file_exists_and_not_empty(filepath)
    local f = io.open(filepath, "rb")
    if not f then return false end
    local size = f:seek("end")
    f:close()
    return size and size > 0
end

local function remove_file(filepath)
    os.remove(filepath)
end

print("========================================")
print("Running pLua Unit Tests")
print("========================================")

-- 1. Test Module Loading
print("[Test 1] Module Loading...")
local ok, plua = pcall(require, "libplua")
assert(ok, "Failed to load libplua module: " .. tostring(plua))
assert(type(plua) == "table", "Expected libplua to return a table")
assert(type(plua.start) == "function", "Missing function plua.start")
assert(type(plua.stop) == "function", "Missing function plua.stop")
assert(type(plua.start_mem) == "function", "Missing function plua.start_mem")
assert(type(plua.stop_mem) == "function", "Missing function plua.stop_mem")
print("  -> Passed: libplua module and API functions loaded successfully")

-- 2. Test CPU Profiling
print("[Test 2] CPU Profiling...")
local cpu_out = "unit_cpu_test.pro"
remove_file(cpu_out)

local function fib(n)
    if n <= 1 then return n end
    return fib(n - 1) + fib(n - 2)
end

local function cpu_work()
    local sum = 0
    for i = 1, 5000 do
        sum = sum + math.sqrt(i)
    end
    fib(22)
    return sum
end

local ret = plua.start(0, cpu_out)
assert(ret == 0, "plua.start returned error: " .. tostring(ret))

for i = 1, 50 do
    cpu_work()
end

ret = plua.stop()
assert(ret == 0, "plua.stop returned error: " .. tostring(ret))

assert(file_exists_and_not_empty(cpu_out), "CPU profile file was not created or is empty: " .. cpu_out)
print("  -> Passed: CPU profile generated successfully (" .. cpu_out .. ")")

-- 3. Test Memory Profiling
print("[Test 3] Memory Profiling...")
local mem_out = "unit_mem_test.pro"
local mem_alloc = "unit_mem_test_ALLOC_SIZE.pro"
local mem_usage = "unit_mem_test_USAGE.pro"
remove_file(mem_alloc)
remove_file(mem_usage)

local ret_mem = plua.start_mem(0, mem_out)
assert(ret_mem == 0, "plua.start_mem returned error: " .. tostring(ret_mem))

local holder = {}
for i = 1, 50000 do
    table.insert(holder, { index = i, str = "plua_test_allocation_" .. i })
end
collectgarbage("collect")

local stop_ret = plua.stop_mem()
assert(stop_ret == 0, "plua.stop_mem returned error: " .. tostring(stop_ret))

assert(file_exists_and_not_empty(mem_alloc), "Memory ALLOC_SIZE profile not created: " .. mem_alloc)
assert(file_exists_and_not_empty(mem_usage), "Memory USAGE profile not created: " .. mem_usage)
print("  -> Passed: Memory profiles generated successfully (" .. mem_alloc .. ", " .. mem_usage .. ")")

-- 4. Test Guard Against Concurrent Profiling
print("[Test 4] Concurrent Profiling Guards...")
local guard_out = "unit_guard_test.pro"
remove_file(guard_out)

ret = plua.start(0, guard_out)
assert(ret == 0, "Initial plua.start should succeed")

local ret2 = plua.start(0, guard_out)
assert(ret2 == -1, "Secondary plua.start while running should fail with -1, got: " .. tostring(ret2))

plua.stop()
remove_file(guard_out)
print("  -> Passed: Re-entrancy guard correctly rejected concurrent start")

-- Cleanup
remove_file(cpu_out)
remove_file(mem_alloc)
remove_file(mem_usage)
remove_file("plua.log")

print("========================================")
print("ALL UNIT TESTS PASSED SUCCESSFULLY!")
print("========================================")
