daukle.language{
  name = "erroring",
  apply = function(consumer, resolved, text)
    error("boom from a language plugin")
  end,
}
