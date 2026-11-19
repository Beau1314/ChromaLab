#!/usr/bin/env python3
"""Build a synthetic Capture One catalog for the spklib reader.

The container follows the observed Windows layout (a plain folder holding
Name.cocatalogdb + Originals/ + Cache/), the schema follows the Core
Data-style tables observed from real catalogs (ZIMAGE/ZVARIANT/ZCOLLECTION/
... with Z_PK/Z_ENT keys and entity names in ZENTITIES). Column guesses are
iterated against spklib-host's capture_one.probe, whose unsupported error
names what is missing.

Referenced (not managed) photos: the RAWs in D:/spk-fixtures/raw.
"""
import json, os, shutil, sqlite3, sys

ROOT = r"D:\spk-fixtures\c1-sample"
CATALOG = os.path.join(ROOT, "MyCatalog.cocatalog")
DB = os.path.join(CATALOG, "MyCatalog.cocatalogdb")
RAWS = sorted(
    os.path.join(r"D:\spk-fixtures\raw", n)
    for n in os.listdir(r"D:\spk-fixtures\raw")
    if n.lower().endswith(".cr2") and "garbage" not in n
)
T0 = 1734000000  # a plausible capture time (2024-12-12, unix seconds)

shutil.rmtree(ROOT, ignore_errors=True)
os.makedirs(os.path.join(CATALOG, "Originals"), exist_ok=True)
os.makedirs(os.path.join(CATALOG, "Cache"), exist_ok=True)

con = sqlite3.connect(DB)
c = con.cursor()
c.executescript(
    """
    CREATE TABLE ZENTITIES (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER, ZNAME TEXT);
    CREATE TABLE ZVERSIONINFO (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZVERSION INTEGER, ZCOMPATIBLEVERSION INTEGER, ZAPPVERSION TEXT,
        ZLASTOPENEDDATE REAL, ZFORMAT TEXT);
    CREATE TABLE ZDOCUMENTCONTENT (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZDOCUMENTUUID TEXT, ZDOCUMENTNAME TEXT, ZDOCUMENTTYPE INTEGER,
        ZROOTCOLLECTION INTEGER, ZLASTIMPORTSCOLLECTION INTEGER,
        ZLASTCAPTURESCOLLECTION INTEGER);
    CREATE TABLE ZPATHLOCATION (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZISRELATIVE INTEGER, ZRELATIVEPATH TEXT, ZMACROOT TEXT, ZWINROOT TEXT,
        ZVOLUME TEXT);
    CREATE TABLE ZIMAGE (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZNAME TEXT, ZIMAGEFILENAME TEXT, ZIMAGELOCATION INTEGER,
        ZFILESIZE INTEGER, ZPIXELWIDTH INTEGER, ZPIXELHEIGHT INTEGER,
        ZCAPTUREDATE REAL, ZCAPTURETIME REAL, ZINTRASH INTEGER, ZISINSIDECATALOG INTEGER,
        ZSIDECARPATH TEXT, ZIMAGEUUID TEXT);
    CREATE TABLE ZVARIANT (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZIMAGE INTEGER, ZINDEX INTEGER, ZVARIANTUUID TEXT,
        ZCOMBINEDSETTINGS INTEGER);
    CREATE TABLE ZVARIANTLAYER (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZVARIANT INTEGER, ZLAYER INTEGER, ZNAME TEXT);
    CREATE TABLE ZVARIANTMETADATA (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZVARIANT INTEGER, ZLAYER INTEGER, ZBASIC_RATING INTEGER,
        ZCOLOR_TAG_INDEX INTEGER, ZKEYWORDS TEXT);
    CREATE TABLE ZCOLLECTION (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZPARENT INTEGER, ZNAME TEXT, ZPOSITION INTEGER, ZUUID TEXT);
    CREATE TABLE ZIMAGEINCOLLECTION (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZCOLLECTION INTEGER, ZIMAGE INTEGER);
    CREATE TABLE ZKEYWORD (Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER,
        ZNAME TEXT, ZPARENT INTEGER, ZLEFT INTEGER, ZRIGHT INTEGER);
    """
)

# Entities by name (numbers are arbitrary but consistent).
entities = [
    ("ProjectCollection", 10), ("VirtualFolderCollection", 11),
    ("AlbumCollection", 12), ("CatalogFolderCollection", 13),
    ("Image", 20), ("Variant", 21), ("VariantLayer", 22),
    ("VariantMetadata", 23), ("PathLocation", 24), ("VersionInfo", 25),
    ("CollectionMembers", 26), ("Keyword", 27),
]
for name, ent in entities:
    # The reader keys the map on Z_ENT (SELECT Z_ENT, ZNAME FROM ZENTITIES).
    c.execute("INSERT INTO ZENTITIES VALUES (?,?,?)", (ent, ent, name))

# Schema 1621 (what 16.3 writes), one version-history row.
c.execute(
    "INSERT INTO ZVERSIONINFO VALUES (?,?,?,?,?,?,?)",
    (1, 25, 1621, 1621, "16.3.9.9", float(T0), "16.3.9.9 Pro Windows"),
)
c.execute(
    "INSERT INTO ZDOCUMENTCONTENT VALUES (?,?,?,?,?,?,?,?)",
    (1, 28, "11111111-2222-3333-4444-555555555555", "My Catalog", 0, 1, 2, 2),
)

# One absolute Windows location for the referenced RAWs.
c.execute(
    "INSERT INTO ZPATHLOCATION VALUES (?,?,?,?,?,?,?)",
    (1, 24, 0, None, r"D:/spk-fixtures/raw", None, None),
)

# The images (referenced, not managed), one variant each, ratings/colours.
images = []
for i, raw in enumerate(RAWS, start=1):
    name = os.path.basename(raw)
    c.execute(
        "INSERT INTO ZIMAGE VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (i, 20, name, name, 1, 28000000, 5760, 3840, float(T0 + i * 60),
         float(T0 + i * 60), 0, 0,
         f"sidecar{i}", f"aaaaaaaa-bbbb-cccc-dddd-{i:012d}"),
    )
    c.execute(
        "INSERT INTO ZVARIANT VALUES (?,?,?,?,?,?)",
        (i, 21, i, 0, f"vvvvvvvv-wwww-xxxx-yyyy-{i:012d}", i),
    )
    # combined layer (the effective settings) only: enough for reads.
    c.execute(
        "INSERT INTO ZVARIANTLAYER VALUES (?,?,?,?,?)",
        (i, 22, i, 2, "combined"),
    )
    c.execute(
        "INSERT INTO ZVARIANTMETADATA VALUES (?,?,?,?,?,?,?)",
        (i, 23, i, i, (i % 6), [0, 1, 3, 4, 5][i % 5], "shoot|outdoor"),
    )
    images.append(i)

# The collection tree: a project holding one album, plus the members.
c.execute("INSERT INTO ZCOLLECTION VALUES (?,?,?,?,?,?)",
          (1, 10, -1, "My Project", 0, "proj-uuid"))
c.execute("INSERT INTO ZCOLLECTION VALUES (?,?,?,?,?,?)",
          (2, 11, 1, "Recent Imports", 0, "recent-uuid"))
c.execute("INSERT INTO ZCOLLECTION VALUES (?,?,?,?,?,?)",
          (3, 12, 1, "Best of shoot", 1, "album-uuid"))
for pos, i in enumerate(images):
    c.execute("INSERT INTO ZIMAGEINCOLLECTION VALUES (?,?,?,?)",
              (pos + 1, 26, 3, i))

con.commit()
con.close()
print(f"synthetic catalog: {CATALOG}")
print(f"  db: {os.path.getsize(DB)} bytes, images: {len(RAWS)}")
