# LRCAT facts (from research)
- Numbers stored as REAL -> read double, round. rating NULL=unrated. pick -1 reject/0/1 pick.
- colorLabels localized text ('' none); English Red/Yellow/Green/Blue/Purple.
- orientation codes: AB=1 BA=2 CD=3 DC=4 AD=5? BC=6 CB=7? DA=8
- Path: AgLibraryRootFolder.absolutePath || AgLibraryFolder.pathFromRoot || AgLibraryFile.idx_filename (forward slashes, trailing slash)
  join: Adobe_images.rootFile -> AgLibraryFile.id_local; file.folder -> AgLibraryFolder.id_local; folder.rootFolder -> AgLibraryRootFolder.id_local
  relativePathFromCatalog may help if drive letters changed.
- Virtual copy: Adobe_images.masterImage != NULL, copyName, same rootFile.
- Keywords: AgLibraryKeyword(id_local,name,parent) root has NULL name/parent; AgLibraryKeywordImage(image, tag)
- Collections: AgLibraryCollection(id_local, creationId, name, parent, systemOnly); creationId com.adobe.ag.library.collection / .smart_collection / .group
  AgLibraryCollectionImage(collection, image). quick collection: systemOnly=1 name 'quick collection'
- Caption/copyright: AgLibraryIPTC(image, caption, copyright). Title only in XMP dc:title.
- GPS: AgHarvestedExifMetadata(image, hasGPS, gpsLatitude, gpsLongitude)
- Develop: Adobe_imageDevelopSettings(image, text) plain Lua 's = { Key = value, ... }' (join Adobe_images.developSettingsIDCache = id_local preferred)
  curves flat arrays {x0,y0,...} 0..255. may be BLOB -> check type
- Adobe_AdditionalMetadata(image, xmp): LR9+ BLOB = 4-byte BE length + zlib stream; older plain text. Contains crs: keys + dc:title etc.
- Stacks: AgLibraryFolderStackImage(image, position, stack)
- Version: Adobe_variablesTable name='Adobe_DBVersion'
- WAL: copy .lrcat (+ -wal/-shm) to temp before open.
- ColorGrade: ColorGradeMidtoneHue/Sat/Lum, ColorGradeShadowLum, ColorGradeHighlightLum, ColorGradeGlobalHue/Sat/Lum, ColorGradeBlending;
  shadows/highlights hue/sat via SplitToningShadowHue/Saturation, SplitToningHighlightHue/Saturation, SplitToningBalance
- Non-raw WB: IncrementalTemperature/IncrementalTint
