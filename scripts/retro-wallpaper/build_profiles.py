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
    "Pope Benedict XVI 2005": "BentoXVI-30-10052007.jpg",
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


# ------------------------------------------------------------ event sets
# (caption, date, commons search query)
EVENTS = {
    2000: [
        ("Sydney dazzles the world with the 2000 Summer Olympics.", "Sep 2000",
         "Sydney 2000 Olympic Games opening ceremony"),
        ("Concorde crashes outside Paris, grounding the supersonic dream.", "Jul 2000",
         "Air France Concorde aircraft"),
        ("Suicide bombers cripple the USS Cole in Aden harbor.", "Oct 2000",
         "USS Cole DDG-67 bombing damage"),
        ("The Space Station takes in its first resident crew.", "Nov 2000",
         "Expedition 1 International Space Station crew"),
        ("Sony's PlayStation 2 launches a new console era.", "Mar 2000",
         "PlayStation 2 console"),
        ("Vladimir Putin is sworn in as Russia's president.", "May 2000",
         "Vladimir Putin 2000 inauguration"),
    ],
    2005: [
        ("Hurricane Katrina drowns New Orleans.", "Aug 2005",
         "Hurricane Katrina 2005 satellite"),
        ("NASA's Deep Impact smashes a probe into a comet.", "Jul 2005",
         "Deep Impact comet Tempel 1"),
        ("Cardinal Ratzinger becomes Pope Benedict XVI.", "Apr 2005",
         "Pope Benedict XVI 2005"),
        ("Microsoft's Xbox 360 opens the HD console age.", "Nov 2005",
         "Xbox 360 console"),
        ("Angela Merkel becomes Germany's first woman Chancellor.", "Nov 2005",
         "Angela Merkel 2005"),
        ("The Airbus A380, the biggest airliner ever, takes flight.", "Apr 2005",
         "Airbus A380 first flight 2005"),
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
}


def games(year):
    return [{"title": t, "note": n} for t, n in GAMES[year]]


def events(year, cache):
    if year in cache:
        return cache[year]
    out = []
    for caption, date, query in EVENTS[year]:
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
]


def main():
    os.makedirs(PROFILES, exist_ok=True)
    ev_cache = {}
    for m in MACHINES:
        sys.stderr.write("Building %s (%s)...\n" % (m["host"], m["hostname"]))
        prof = dict(m)
        prof["games_cpu"] = games(m["cpu_year"])
        prof["games_gpu"] = games(m["gpu_year"])
        prof["events"] = events(m["cpu_year"], ev_cache)
        path = os.path.join(PROFILES, m["host"] + ".json")
        with open(path, "w") as f:
            json.dump(prof, f, indent=2)
        print("wrote", path)


if __name__ == "__main__":
    main()
