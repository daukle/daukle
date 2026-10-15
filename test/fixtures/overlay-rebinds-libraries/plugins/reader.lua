daukle.plugin{ api = 2, uses = {} }

assert(string.format("%d", 1) == "1")
assert(table.concat({ "a", "b" }) == "ab")
assert(math.floor(2.5) == 2)

daukle.language{
  name = "reader",
  apply = function(consumer, resolved, text) return text end,
}
