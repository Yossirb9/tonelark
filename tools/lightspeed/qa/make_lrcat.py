"""Build a synthetic Lightroom Classic catalog (schema subset) for testing the Lightspeed importer."""
import os, shutil, sqlite3, struct, zlib, sys

QA = r"C:\lightspeed\qa"
ROOT = os.path.join(QA, "lrtest")
PHOTOS = os.path.join(ROOT, "Photos")
CATDIR = os.path.join(ROOT, "Lightroom")
CAT = os.path.join(CATDIR, "Test Catalog.lrcat")

if os.path.exists(ROOT):
    shutil.rmtree(ROOT)
os.makedirs(os.path.join(PHOTOS, "2024", "Milos"))
os.makedirs(os.path.join(PHOTOS, "2025", "Charts"))
os.makedirs(CATDIR)

src = os.path.join(r"C:\lightspeed\darktable\src\tests\integration\images")
shutil.copy(os.path.join(src, "hlrecovery.arw"), os.path.join(PHOTOS, "2024", "Milos", "DSC00001.ARW"))
shutil.copy(os.path.join(src, "xtransIV.raf"), os.path.join(PHOTOS, "2024", "Milos", "DSCF0002.RAF"))
shutil.copy(os.path.join(src, "mire1.cr2"), os.path.join(PHOTOS, "2025", "Charts", "IMG_0003.CR2"))
shutil.copy(os.path.join(QA, "base_hl.jpg"), os.path.join(PHOTOS, "2025", "Charts", "IMG_0004.JPG"))

db = sqlite3.connect(CAT)
c = db.cursor()
c.executescript("""
CREATE TABLE Adobe_variablesTable (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, name, type, value NOT NULL DEFAULT '');
CREATE TABLE AgLibraryRootFolder (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, absolutePath UNIQUE NOT NULL DEFAULT '', name NOT NULL DEFAULT '', relativePathFromCatalog);
CREATE TABLE AgLibraryFolder (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, parentId INTEGER, pathFromRoot NOT NULL DEFAULT '', rootFolder INTEGER NOT NULL DEFAULT 0, visibility INTEGER);
CREATE TABLE AgLibraryFile (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, baseName NOT NULL DEFAULT '', errorMessage, errorTime, extension NOT NULL DEFAULT '', externalModTime, folder INTEGER NOT NULL DEFAULT 0, idx_filename NOT NULL DEFAULT '', importHash, lc_idx_filename, lc_idx_filenameExtension, md5, modTime, originalFilename NOT NULL DEFAULT '', sidecarExtensions);
CREATE TABLE Adobe_images (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, aspectRatioCache NOT NULL DEFAULT -1, bitDepth NOT NULL DEFAULT 0, captureTime, colorChannels NOT NULL DEFAULT 0, colorLabels NOT NULL DEFAULT '', colorMode NOT NULL DEFAULT -1, copyCreationTime NOT NULL DEFAULT -63113817600, copyName, copyReason, developSettingsIDCache, editLock INTEGER NOT NULL DEFAULT 0, fileFormat NOT NULL DEFAULT 'unset', fileHeight, fileWidth, hasMissingSidecars INTEGER, masterImage INTEGER, orientation, originalCaptureTime, originalRootEntity, panningDistanceH, panningDistanceV, pick NOT NULL DEFAULT 0, positionInFolder NOT NULL DEFAULT 'z', propertiesCache, pyramidIDCache, rating, rootFile INTEGER NOT NULL DEFAULT 0, sidecarStatus, touchCount NOT NULL DEFAULT 0, touchTime NOT NULL DEFAULT 0);
CREATE TABLE Adobe_imageDevelopSettings (id_local INTEGER PRIMARY KEY, allowFastRender INTEGER, beforeSettingsIDCache, croppedHeight, croppedWidth, digest, fileHeight, fileWidth, grayscale INTEGER, hasDevelopAdjustments INTEGER, hasDevelopAdjustmentsEx, historySettingsID, image INTEGER, processVersion, settingsID, snapshotID, text, validatedForVersion, whiteBalance);
CREATE TABLE Adobe_AdditionalMetadata (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, additionalInfoSet INTEGER NOT NULL DEFAULT 0, embeddedXmp INTEGER NOT NULL DEFAULT 0, externalXmpIsDirty INTEGER NOT NULL DEFAULT 0, image INTEGER, incrementalWhiteBalance INTEGER NOT NULL DEFAULT 0, internalXmpDigest, isRawFile INTEGER NOT NULL DEFAULT 0, lastSynchronizedHash, lastSynchronizedTimestamp NOT NULL DEFAULT -63113817600, metadataPresetID, metadataVersion, monochrome INTEGER NOT NULL DEFAULT 0, xmp NOT NULL DEFAULT '');
CREATE TABLE AgLibraryKeyword (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, dateCreated NOT NULL DEFAULT '', genealogy NOT NULL DEFAULT '', imageCountCache DEFAULT -1, includeOnExport INTEGER NOT NULL DEFAULT 1, includeParents INTEGER NOT NULL DEFAULT 1, includeSynonyms INTEGER NOT NULL DEFAULT 1, keywordType, lastApplied, lc_name, name, parent INTEGER);
CREATE TABLE AgLibraryKeywordImage (id_local INTEGER PRIMARY KEY, image INTEGER NOT NULL DEFAULT 0, tag INTEGER NOT NULL DEFAULT 0);
CREATE TABLE AgLibraryCollection (id_local INTEGER PRIMARY KEY, creationId NOT NULL DEFAULT '', genealogy NOT NULL DEFAULT '', imageCount, name NOT NULL DEFAULT '', parent INTEGER, systemOnly NOT NULL DEFAULT '');
CREATE TABLE AgLibraryCollectionImage (id_local INTEGER PRIMARY KEY, collection INTEGER NOT NULL DEFAULT 0, image INTEGER NOT NULL DEFAULT 0, pick NOT NULL DEFAULT 0, positionInCollection);
CREATE TABLE AgLibraryIPTC (id_local INTEGER PRIMARY KEY, altTextAccessibility, caption, copyright, extDescrAccessibility, image INTEGER NOT NULL DEFAULT 0);
CREATE TABLE AgHarvestedExifMetadata (id_local INTEGER PRIMARY KEY, image INTEGER, aperture, cameraModelRef INTEGER, cameraSNRef INTEGER, dateDay, dateMonth, dateYear, flashFired INTEGER, focalLength, gpsLatitude, gpsLongitude, gpsSequence NOT NULL DEFAULT 0, hasGPS INTEGER, isoSpeedRating, lensRef INTEGER, shutterSpeed);
CREATE TABLE AgLibraryFolderStack (id_local INTEGER PRIMARY KEY, id_global UNIQUE NOT NULL, collapsed INTEGER NOT NULL DEFAULT 0, text NOT NULL DEFAULT '');
CREATE TABLE AgLibraryFolderStackImage (id_local INTEGER PRIMARY KEY, collapsed INTEGER NOT NULL DEFAULT 0, image INTEGER NOT NULL DEFAULT 0, position NOT NULL DEFAULT '', stack INTEGER NOT NULL DEFAULT 0);
""")
c.execute("INSERT INTO Adobe_variablesTable VALUES (1,'g1','Adobe_DBVersion',NULL,'1300025')")

photos_uri = PHOTOS.replace("\\", "/") + "/"
c.execute("INSERT INTO AgLibraryRootFolder VALUES (1,'r1',?,?,?)", (photos_uri, "Photos", "../Photos/"))
c.execute("INSERT INTO AgLibraryRootFolder VALUES (2,'r2','E:/Old Drive/Archive/','Archive',NULL)")
for fid, path, root in [(1, "", 1), (2, "2024/", 1), (3, "2024/Milos/", 1), (4, "2025/", 1), (5, "2025/Charts/", 1), (6, "", 2), (7, "2019/", 2)]:
    c.execute("INSERT INTO AgLibraryFolder VALUES (?,?,NULL,?,?,NULL)", (fid, "f%d" % fid, path, root))
files = [(1, "DSC00001", "ARW", 3), (2, "DSCF0002", "RAF", 3), (3, "IMG_0003", "CR2", 5), (4, "IMG_0004", "JPG", 5), (5, "OLD_0005", "NEF", 7)]
for fid, base, ext, folder in files:
    c.execute("INSERT INTO AgLibraryFile (id_local,id_global,baseName,extension,folder,idx_filename,originalFilename) VALUES (?,?,?,?,?,?,?)",
              (fid, "file%d" % fid, base, ext, folder, base + "." + ext, base + "." + ext))

def lua(d):
    out = ["s = { "]
    for k in sorted(d):
        v = d[k]
        if isinstance(v, bool):
            s = "true" if v else "false"
        elif isinstance(v, (int, float)):
            s = repr(v)
        elif isinstance(v, str):
            s = '"' + v.replace('"', '\\"') + '"'
        elif isinstance(v, list):
            s = "{ " + ",\n".join(repr(x) for x in v) + ",\n }"
        out.append("%s = %s,\n" % (k, s))
    out.append("}")
    return "".join(out)

default = dict(Version="16.0", ProcessVersion="15.4", WhiteBalance="As Shot", Sharpness=40, SharpenRadius=1.0,
               ColorNoiseReduction=25, CameraProfile="Adobe Standard", ToneCurveName2012="Linear",
               ToneCurvePV2012=[0, 0, 255, 255], HasCrop=False, ConvertToGrayscale=False, LensProfileEnable=0)

img1 = dict(default, Exposure2012=0.5, Contrast2012=20, Highlights2012=-60, Shadows2012=40, Whites2012=10,
            Blacks2012=-15, Clarity2012=25, Texture=15, Dehaze=10, Vibrance=30, Saturation=5,
            WhiteBalance="Custom", Temperature=5600, Tint=8, HasCrop=True, CropLeft=0.08, CropTop=0.06,
            CropRight=0.94, CropBottom=0.96, CropAngle=1.2, ToneCurveName2012="Medium Contrast",
            ToneCurvePV2012=[0, 0, 32, 22, 64, 56, 128, 128, 192, 196, 255, 255],
            SaturationAdjustmentBlue=-20, LuminanceAdjustmentBlue=-15, HueAdjustmentOrange=5,
            SplitToningShadowHue=210, SplitToningShadowSaturation=15, SplitToningHighlightHue=45,
            SplitToningHighlightSaturation=20, PostCropVignetteAmount=-20, PostCropVignetteMidpoint=50,
            PostCropVignetteFeather=50, PostCropVignetteRoundness=0, PostCropVignetteStyle=1, LensProfileEnable=1)
vc1 = dict(default, ConvertToGrayscale=True, Exposure2012=0.3, Contrast2012=40, Clarity2012=30)
img2 = dict(default)  # untouched
img3 = dict(default, Exposure2012=-0.3)
img4 = dict(default, Exposure2012=0.2, WhiteBalance="Custom", IncrementalTemperature=20, IncrementalTint=-10)
img5 = dict(default, Exposure2012=1.0)

# id, rootFile, master, copyName, rating, pick, label, orientation, settings, w, h, format
images = [
    (101, 1, None, None, 4.0, 1.0, "Red", "AB", img1, 5504, 3672, "RAW"),
    (102, 2, None, None, 2.0, -1.0, "Green", "AB", img2, 6240, 4160, "RAW"),
    (103, 3, None, None, None, 0.0, "Yellow", "BC", img3, 3908, 2600, "RAW"),
    (104, 4, None, None, 3.0, 0.0, "", "AB", img4, 5492, 3672, "JPG"),
    (105, 5, None, None, 5.0, 0.0, "Purple", "AB", img5, 6000, 4000, "RAW"),
    (106, 1, 101, "B&W", 5.0, 1.0, "Blue", "AB", vc1, 5504, 3672, "RAW"),
]
for n, (iid, rf, master, copyname, rating, pick, label, orient, settings, w, h, fmt) in enumerate(images):
    dsid = 1000 + iid
    c.execute("INSERT INTO Adobe_images (id_local,id_global,captureTime,colorLabels,copyName,developSettingsIDCache,fileFormat,fileHeight,fileWidth,masterImage,orientation,pick,rating,rootFile) "
              "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
              (iid, "img%d" % iid, "2024-06-01T10:00:%02d" % n, label, copyname, dsid, fmt, float(h), float(w), master, orient, pick, rating, rf))
    c.execute("INSERT INTO Adobe_imageDevelopSettings (id_local,image,text,processVersion,whiteBalance) VALUES (?,?,?,?,?)",
              (dsid, iid, lua(settings), "15.4", settings.get("WhiteBalance")))

def xmp(title, creator, extra_crs=""):
    return ('<?xpacket begin="\ufeff" id="W5M0MpCehiHzreSzNTczkc9d"?>\n'
            '<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Adobe XMP Core 7.0">\n'
            ' <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">\n'
            '  <rdf:Description rdf:about=""\n'
            '    xmlns:dc="http://purl.org/dc/elements/1.1/"\n'
            '    xmlns:xmpMM="http://ns.adobe.com/xap/1.0/mm/"\n'
            '    xmlns:stEvt="http://ns.adobe.com/xap/1.0/sType/ResourceEvent#"\n'
            '    xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"' + extra_crs + '>\n'
            '   <dc:title>\n    <rdf:Alt>\n     <rdf:li xml:lang="x-default">' + title + '</rdf:li>\n    </rdf:Alt>\n   </dc:title>\n'
            '   <dc:creator>\n    <rdf:Seq>\n     <rdf:li>' + creator + '</rdf:li>\n    </rdf:Seq>\n   </dc:creator>\n'
            '   <xmpMM:History>\n    <rdf:Seq>\n     <rdf:li stEvt:action="saved" stEvt:softwareAgent="Adobe Photoshop Lightroom Classic 13.0 (Windows)"/>\n    </rdf:Seq>\n   </xmpMM:History>\n'
            '  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end="w"?>')

x1 = xmp("Milos harbor", "Yossi", '\n    crs:Exposure2012="+0.50"')
blob = struct.pack(">I", len(x1.encode("utf-8"))) + zlib.compress(x1.encode("utf-8"))
c.execute("INSERT INTO Adobe_AdditionalMetadata (id_local,id_global,image,xmp) VALUES (1,'am1',101,?)", (sqlite3.Binary(blob),))
c.execute("INSERT INTO Adobe_AdditionalMetadata (id_local,id_global,image,xmp) VALUES (2,'am3',103,?)", (xmp("Test chart", "Lightspeed QA"),))

# keywords: root, Places > Greece > Milos, Boats, Test chart
kw = [(1, None, None), (2, "Places", 1), (3, "Greece", 2), (4, "Milos", 3), (5, "Boats", 1), (6, "Test chart", 1)]
for kid, name, parent in kw:
    c.execute("INSERT INTO AgLibraryKeyword (id_local,id_global,name,lc_name,parent) VALUES (?,?,?,?,?)",
              (kid, "k%d" % kid, name, name.lower() if name else None, parent))
for n, (image, tag) in enumerate([(101, 4), (101, 5), (106, 4), (103, 6)]):
    c.execute("INSERT INTO AgLibraryKeywordImage VALUES (?,?,?)", (n + 1, image, tag))

cols = [(1, "com.adobe.ag.library.group", "Portfolio", None, 0), (2, "com.adobe.ag.library.collection", "Best of 2024", 1, 0),
        (3, "com.adobe.ag.library.collection", "quick collection", None, 1), (4, "com.adobe.ag.library.smart_collection", "Five stars", None, 0)]
for cid, creation, name, parent, system in cols:
    c.execute("INSERT INTO AgLibraryCollection (id_local,creationId,name,parent,systemOnly) VALUES (?,?,?,?,?)", (cid, creation, name, parent, system))
for n, (col, image) in enumerate([(2, 101), (2, 106), (2, 104), (3, 103)]):
    c.execute("INSERT INTO AgLibraryCollectionImage (id_local,collection,image) VALUES (?,?,?)", (n + 1, col, image))

c.execute("INSERT INTO AgLibraryIPTC (id_local,caption,copyright,image) VALUES (1,'Fishing boats in Adamas port','(c) 2024 Yossi',101)")
c.execute("INSERT INTO AgHarvestedExifMetadata (id_local,image,hasGPS,gpsLatitude,gpsLongitude) VALUES (1,101,1,36.7236,24.4471)")

c.execute("INSERT INTO AgLibraryFolderStack VALUES (1,'s1',1,'')")
c.execute("INSERT INTO AgLibraryFolderStackImage VALUES (1,1,101,1.0,1)")
c.execute("INSERT INTO AgLibraryFolderStackImage VALUES (2,1,102,2.0,1)")

db.commit()
db.close()
print("catalog written:", CAT)
