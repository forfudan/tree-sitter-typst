= Overview <top>
// <- markup.heading.marker
//^ markup.heading.1
//          ^ label

Some *bold* and _slanted_ words, `code`, and https://typst.app here.
//    ^ markup.bold
//               ^ markup.italic
//                                ^ markup.raw.inline
//                                                   ^ markup.link.url

- a bullet
// <- markup.list.unnumbered
+ a number
// <- markup.list.numbered

See @top and \#hash.
//   ^ markup.link.label
//           ^ constant.character.escape
