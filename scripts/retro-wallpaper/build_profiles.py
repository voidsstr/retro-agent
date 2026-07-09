#!/usr/bin/env python3
"""
Build wallpaper profile JSONs for the connected retro fleet.

Specs were gathered live from each agent (SYSINFO/VIDEODIAG/registry). Event
collage images are resolved through the Wikimedia Commons API so every image_url
is a real, current thumbnail (no hand-guessed hash paths that 404).
"""
import json, os, sys, urllib.request, urllib.parse, html

HERE = os.path.dirname(os.path.abspath(__file__))
PROFILES = os.path.join(HERE, "profiles")

API = "https://commons.wikimedia.org/w/api.php"

# Some event searches return the wrong top hit (a flag, a Vita, a lightning
# photo). Pin those to an exact, verified Commons filename instead.
OVERRIDE = {
    "Expedition 1 International Space Station crew": "STS-97 ISS.jpg",
    "PlayStation 2 console": "Sony-PlayStation-2-30001-wController-L.jpg",
    "Intel Pentium 4 processor": "KL Intel Pentium 4 Northwood.jpg",
    "NVIDIA GeForce2 graphics card": "Geforce2gts.jpg",
    "Mars Reconnaissance Orbiter": "Mars Reconnaissance Orbiter.jpg",
    "YouTube": "Logo of YouTube (2005-2011).svg",
    "Power Mac G4 Cube": "Power Mac G4 Cube.jpg",
}


def _imageinfo(params):
    url = API + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={
        "User-Agent": "retro-wallpaper-bot/1.0 (perrymb@gmail.com)"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def _credit_from(ii):
    import re
    meta = ii.get("extmetadata", {})
    artist = meta.get("Artist", {}).get("value", "")
    lic = meta.get("LicenseShortName", {}).get("value", "")
    artist = re.sub(r"<[^>]+>", "", html.unescape(artist)).strip()
    artist = re.sub(r"\s+", " ", artist)[:40]
    return ", ".join(x for x in (artist, lic) if x)[:60]


def commons_by_title(filename, width=1024):
    data = _imageinfo({
        "action": "query", "format": "json",
        "titles": "File:" + filename,
        "prop": "imageinfo", "iiprop": "url|extmetadata",
        "iiurlwidth": str(width),
    })
    page = next(iter(data["query"]["pages"].values()))
    ii = page["imageinfo"][0]
    return (ii.get("thumburl") or ii["url"]), _credit_from(ii)


def commons_image(query, width=1024):
    if query in OVERRIDE:
        return commons_by_title(OVERRIDE[query], width)
    """Return (thumb_url, credit) for the best Commons image match for query."""
    params = {
        "action": "query", "format": "json",
        "generator": "search", "gsrsearch": query,
        "gsrnamespace": "6", "gsrlimit": "1",
        "prop": "imageinfo", "iiprop": "url|extmetadata",
        "iiurlwidth": str(width),
    }
    data = _imageinfo(params)
    pages = data.get("query", {}).get("pages", {})
    if not pages:
        raise RuntimeError("no match for %r" % query)
    page = next(iter(pages.values()))
    ii = page["imageinfo"][0]
    thumb = ii.get("thumburl") or ii["url"]
    return thumb, _credit_from(ii)


# ------------------------------------------------------------ tech/research pools
# 12 tech + science-research milestones per era. Each wallpaper iteration shows a
# rotating 6-item window of the pool (see build_iterations), so the collage cycles
# through different tech content for that machine's year.
# (caption, date, commons search query)
TECH = {
    2000: [
        ("Sony's PlayStation 2 redraws the console landscape.", "Mar 2000",
         "PlayStation 2 console"),
        ("The Human Genome Project unveils its first draft.", "Jun 2000",
         "DNA double helix structure"),
        ("AMD's Athlon is the first CPU to reach 1 GHz.", "Mar 2000",
         "AMD Athlon Slot A processor"),
        ("Intel answers with the Pentium 4 and NetBurst.", "Nov 2000",
         "Intel Pentium 4 processor"),
        ("NVIDIA ships the GeForce2 GTS graphics chip.", "Apr 2000",
         "NVIDIA GeForce2 graphics card"),
        ("The Nokia 3310 becomes a mobile-phone legend.", "Sep 2000",
         "Nokia 3310"),
        ("USB flash drives start replacing the floppy disk.", "2000",
         "USB flash drive"),
        ("Sony's AIBO robotic dog learns new tricks.", "2000",
         "Sony AIBO robot"),
        ("The Space Station takes in its first resident crew.", "Nov 2000",
         "Expedition 1 International Space Station crew"),
        ("Apple's striking Power Mac G4 Cube goes on sale.", "Jul 2000",
         "Power Mac G4 Cube"),
        ("Windows 2000 targets the enterprise desktop.", "Feb 2000",
         "Windows 2000"),
        ("Toyota's Prius brings hybrid cars to the world.", "2000",
         "Toyota Prius first generation"),
    ],
    2005: [
        ("Microsoft's Xbox 360 opens the HD-console era.", "Nov 2005",
         "Xbox 360 console"),
        ("YouTube uploads its first video and remakes media.", "Apr 2005",
         "YouTube"),
        ("Huygens becomes the first probe to land on Titan.", "Jan 2005",
         "Huygens probe Titan surface"),
        ("Apple's iPod nano reinvents the music player.", "Sep 2005",
         "iPod nano 1st generation"),
        ("Sony's PSP puts console gaming in your pocket.", "Mar 2005",
         "PlayStation Portable PSP"),
        ("Google Earth puts the whole planet on your screen.", "Jun 2005",
         "Google Earth globe"),
        ("NASA launches the Mars Reconnaissance Orbiter.", "Aug 2005",
         "Mars Reconnaissance Orbiter"),
        ("AMD ships the dual-core Athlon 64 X2.", "May 2005",
         "AMD Athlon 64 X2 processor"),
        ("NVIDIA's GeForce 7800 GTX raises the GPU bar.", "Jun 2005",
         "GeForce 7800 GTX"),
        ("Deep Impact slams a probe into comet Tempel 1.", "Jul 2005",
         "Deep Impact comet Tempel 1"),
        ("Astronomers announce the dwarf planet Eris.", "2005",
         "Eris dwarf planet"),
        ("Steve Jobs tells Stanford to stay hungry.", "Jun 2005",
         "Steve Jobs 2005"),
    ],
    2011: [
        ("IBM's Watson beats the champions at Jeopardy!", "Feb 2011",
         "IBM Watson computer Jeopardy"),
        ("NASA launches Curiosity on its way to Mars.", "Nov 2011",
         "Curiosity rover Mars Science Laboratory"),
        ("Apple's iPhone 4S introduces the Siri assistant.", "Oct 2011",
         "iPhone 4S"),
        ("Intel's Sandy Bridge Core chips define the era.", "Jan 2011",
         "Sandy Bridge die"),
        ("Kepler confirms its first habitable-zone planet.", "Dec 2011",
         "Kepler space telescope"),
        ("The Raspberry Pi promises a 25-dollar computer.", "2011",
         "Raspberry Pi board"),
        ("Nintendo's 3DS brings glasses-free 3D gaming.", "Feb 2011",
         "Nintendo 3DS"),
        ("Bitcoin reaches parity with the US dollar.", "Feb 2011",
         "Bitcoin logo"),
        ("Apple's iPad 2 accelerates the tablet boom.", "Mar 2011",
         "iPad 2"),
        ("CERN's Large Hadron Collider closes in on the Higgs.", "Dec 2011",
         "Large Hadron Collider CMS detector"),
        ("The world says goodbye to Steve Jobs.", "Oct 2011",
         "Steve Jobs"),
        ("Ultrabooks reboot the thin-and-light laptop.", "2011",
         "MacBook Air 2011"),
    ],
}

# ------------------------------------------------------------ games by year
GAMES = {
    2000: [
        ("Diablo II", "loot-driven action-RPG"),
        ("The Sims", "life-sim phenomenon"),
        ("Counter-Strike", "the mod that ate LAN parties"),
        ("Deus Ex", "genre-blending cyberpunk"),
        ("Baldur's Gate II", "landmark D&D RPG"),
        ("Hitman: Codename 47", "stealth assassin debut"),
        ("Final Fantasy IX", "return to fantasy roots"),
        ("Tony Hawk's Pro Skater 2", "arcade skating perfected"),
    ],
    2002: [
        ("GTA: Vice City", "neon-soaked 80s crime"),
        ("Warcraft III", "RTS with hero units"),
        ("TES III: Morrowind", "vast open-world RPG"),
        ("Battlefield 1942", "32-player online war"),
        ("Neverwinter Nights", "D&D toolset RPG"),
        ("Age of Mythology", "myth-powered RTS"),
        ("Hitman 2", "silent assassin returns"),
        ("Metroid Prime", "first-person Metroid"),
    ],
    2004: [
        ("Half-Life 2", "physics-driven FPS classic"),
        ("World of Warcraft", "the MMO juggernaut"),
        ("Doom 3", "survival-horror shooter"),
        ("Halo 2", "console FPS + Xbox Live"),
        ("Far Cry", "open tropical shooter"),
        ("Rome: Total War", "epic real-time battles"),
        ("The Sims 2", "life-sim sequel hit"),
        ("GTA: San Andreas", "sprawling gang epic"),
    ],
    2005: [
        ("F.E.A.R.", "horror FPS with smart AI"),
        ("Civilization IV", "definitive 4X strategy"),
        ("Battlefield 2", "modern warfare online"),
        ("Guild Wars", "no-subscription MMO"),
        ("Quake 4", "id's shooter sequel"),
        ("Age of Empires III", "gunpowder-era RTS"),
        ("Resident Evil 4", "reinvented survival horror"),
        ("Call of Duty 2", "WWII shooter benchmark"),
    ],
    2007: [
        ("Crysis", "the GPU-melting benchmark"),
        ("BioShock", "Rapture's atmospheric FPS"),
        ("Portal", "physics puzzle icon"),
        ("Call of Duty 4", "Modern Warfare arrives"),
        ("Team Fortress 2", "class-based multiplayer"),
        ("S.T.A.L.K.E.R.", "open-world survival FPS"),
        ("The Witcher", "dark choice-driven RPG"),
        ("Mass Effect", "cinematic space RPG"),
    ],
    2011: [
        ("The Elder Scrolls V: Skyrim", "open-world RPG landmark"),
        ("Portal 2", "acclaimed puzzle sequel"),
        ("Batman: Arkham City", "open-world superhero action"),
        ("Deus Ex: Human Revolution", "cyberpunk stealth RPG"),
        ("Dark Souls", "brutal action-RPG milestone"),
        ("Battlefield 3", "Frostbite military shooter"),
        ("The Witcher 2", "dark PC fantasy RPG"),
        ("Minecraft 1.0", "sandbox phenomenon"),
    ],
}


def games(year):
    return [{"title": t, "note": n} for t, n in GAMES[year]]


ITERATIONS = 10   # wallpaper variants per machine
WINDOW = 6        # events shown per wallpaper


def tech_pool(year, cache):
    """Resolve the full 12-item tech/research pool for a year (cached)."""
    if year in cache:
        return cache[year]
    out = []
    for caption, date, query in TECH[year]:
        try:
            img, credit = commons_image(query)
            sys.stderr.write("  [ok]  %s -> %s\n" % (query, img.split("/")[-1][:50]))
        except Exception as e:
            img, credit = None, ""
            sys.stderr.write("  [MISS] %s: %s\n" % (query, e))
        ev = {"caption": caption, "date": date, "credit": credit}
        if img:
            ev["image_url"] = img
        out.append(ev)
    cache[year] = out
    return out


def iteration_events(pool, k):
    """The k-th rotating 6-item window over the pool. Step of 5 (coprime to 12)
    so consecutive iterations share only one item -> each looks distinct."""
    n = len(pool)
    start = (k * 5) % n
    return [pool[(start + j) % n] for j in range(WINDOW)]


# ------------------------------------------------------------ machine profiles
MACHINES = [
    {
        "host": "192.168.1.123", "hostname": "2004-XP",
        "title": "Athlon 64 Gaming Rig - Windows XP",
        "width": 1280, "height": 1024,
        "accent": [90, 170, 255], "accent2": [235, 70, 70],
        "cpu_year": 2005, "gpu_year": 2007,
        "cpu_label": "AMD Athlon 64 4000+ - launched 2005",
        "gpu_label": "ATI Radeon HD 3850 AGP - launched 2007",
        "specs": [
            ["CPU", "Athlon 64 4000+ 2.4GHz"],
            ["GPU", "Radeon HD 3850 AGP"],
            ["RAM", "2048 MB"],
            ["OS", "Windows XP Pro SP3"],
            ["DISPLAY", "1280 x 1024 @ 85Hz 32bpp"],
            ["STORAGE", "238 GB fixed"],
        ],
    },
    {
        "host": "192.168.1.124", "hostname": "ADMIN",
        "title": "Pentium III Workstation - Windows XP",
        "width": 1024, "height": 768,
        "accent": [120, 200, 90], "accent2": [80, 200, 255],
        "cpu_year": 2000, "gpu_year": 2000,
        "cpu_label": "Intel Pentium III - launched 2000",
        "gpu_label": "NVIDIA GeForce2 GTS - launched 2000",
        "specs": [
            ["CPU", "Pentium III 850MHz"],
            ["GPU", "GeForce2 GTS / Pro"],
            ["RAM", "384 MB"],
            ["OS", "Windows XP Pro SP3"],
            ["DISPLAY", "1024 x 768 @ 100Hz 32bpp"],
            ["STORAGE", "128 GB fixed"],
        ],
    },
    {
        "host": "192.168.1.133", "hostname": "P3-DUAL",
        "title": "Dual Pentium III - Windows XP",
        "width": 1024, "height": 768,
        "accent": [80, 200, 255], "accent2": [130, 200, 90],
        "cpu_year": 2000, "gpu_year": 2002,
        "cpu_label": "Dual Intel Pentium III - launched 2000",
        "gpu_label": "NVIDIA GeForce4 Ti 4600 - launched 2002",
        "specs": [
            ["CPU", "2x Pentium III 700MHz"],
            ["GPU", "GeForce4 Ti 4600"],
            ["RAM", "1024 MB"],
            ["OS", "Windows XP Pro SP3"],
            ["DISPLAY", "1024 x 768 @ 100Hz"],
            ["STORAGE", "931 GB fixed"],
        ],
    },
    {
        "host": "192.168.1.143", "hostname": "1GHZ",
        "title": "Athlon 1GHz Rig - Windows XP",
        "width": 1024, "height": 768,
        "accent": [255, 120, 40], "accent2": [120, 200, 90],
        "cpu_year": 2000, "gpu_year": 2004,
        "cpu_label": "AMD Athlon 1GHz - launched 2000",
        "gpu_label": "NVIDIA GeForce 6800 - launched 2004",
        "specs": [
            ["CPU", "Athlon 1.0GHz (K7)"],
            ["GPU", "GeForce 6800"],
            ["RAM", "512 MB"],
            ["OS", "Windows XP Pro SP3"],
            ["DISPLAY", "1024 x 768 @ 85Hz 32bpp"],
            ["STORAGE", "224 GB fixed"],
        ],
    },
    {
        "host": "192.168.1.145", "hostname": "DELL",
        "title": "Sandy Bridge Workstation - Windows XP",
        "width": 1920, "height": 1080,
        "accent": [90, 170, 255], "accent2": [130, 200, 90],
        "cpu_year": 2011, "gpu_year": 2011,
        "cpu_label": "Intel Core i5-2400 - launched 2011",
        "gpu_label": "Intel HD Graphics 2000 - launched 2011",
        "specs": [
            ["CPU", "Core i5-2400 @ 3.10GHz"],
            ["GPU", "Intel HD Graphics 2000"],
            ["RAM", "2048 MB"],
            ["OS", "Windows XP Pro SP3"],
            ["DISPLAY", "1920 x 1080 @ 60Hz 32bpp"],
            ["STORAGE", "224 GB fixed"],
        ],
    },
]


def main():
    os.makedirs(PROFILES, exist_ok=True)
    # clear stale iteration files so removed iterations don't linger
    for f in os.listdir(PROFILES):
        if ".i" in f and f.endswith(".json"):
            os.remove(os.path.join(PROFILES, f))
    pool_cache = {}
    for m in MACHINES:
        sys.stderr.write("Building %s (%s)...\n" % (m["host"], m["hostname"]))
        pool = tech_pool(m["cpu_year"], pool_cache)
        for k in range(ITERATIONS):
            prof = dict(m)
            prof["iteration"] = k
            prof["games_cpu"] = games(m["cpu_year"])
            prof["games_gpu"] = games(m["gpu_year"])
            prof["events"] = iteration_events(pool, k)
            path = os.path.join(PROFILES, "%s.i%02d.json" % (m["host"], k))
            with open(path, "w") as f:
                json.dump(prof, f, indent=2)
        print("wrote %d iterations for %s" % (ITERATIONS, m["host"]))


if __name__ == "__main__":
    main()
