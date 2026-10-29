local count = 0
for _ in pairs(daukle.config.modules) do count = count + 1 end
daukle.config.modules["m" .. count] = {}
