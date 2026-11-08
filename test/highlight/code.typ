#let scale(value, factor: 2) = value * factor
// <- punctuation.special
//^ keyword.storage
//   ^ function
//         ^ variable.parameter
//                ^ variable.parameter
//                        ^ constant.numeric.integer
//                                   ^ operator
//                             ^ variable.parameter

#set text(size: 11pt, lang: "zh")
//^ keyword
//   ^ function
//        ^ variable.parameter
//              ^ constant.numeric.float
//                          ^ string

#show heading.where(level: 1): it => it.body
//^ keyword
//    ^ function
//            ^ function.method
//                             ^ variable.parameter
//                                      ^ variable.other.member

#import "@preview/cetz:0.3.1": canvas
//^ keyword.control.import
//      ^ string.special.path

#if true { none } else { auto }
//^ keyword.control.conditional
//  ^ constant.builtin.boolean
//         ^ constant.builtin
//                ^ keyword.control.conditional

#for x in (1, 2) [#x]
//^ keyword.control.repeat
//     ^ keyword.control.repeat

#(1 + 2) // trailing remark
//       ^ comment.line
