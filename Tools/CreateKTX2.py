import sys
import os
import subprocess
import pathlib
import tempfile
from PIL import Image

DEFAULT_INPUT_FOLDER_NAME = "mat_dev"
DEFAULT_OUTPUT_FOLDER_NAME = "mat"

INPUT_EXTENSIONS = [".tga", ".png"]
OUTPUT_EXTENSION = ".ktx2"

CACHE_FILE_NAME = "CreateKTX2Cache.txt"


def printInPowerShell(msg, color):
    p = subprocess.run([
        "PowerShell",
        "Write-Host",
        "\"" + msg + "\"",
        "-ForegroundColor", color
    ])


def main():
    if "--help" in sys.argv or "--h" in sys.argv or "-help" in sys.argv or "-h" in sys.argv:
        print("Usage: CreateKTX2.py")
        print("")
        print("  CreateKTX2 compresses PNG and TGA files from input folder to KTX2 files with")
        print("  BC7/BC5 format to output folder. Folders of the files are preserved the")
        print("  same as in the input folder.")
        print("")
        print("  If image file name without extension ends with \"_n\", it's assumed that")
        print("  it's a normal map image and will be compressed with BC5 format.")
        print("")
        print("  Requires compressonatorcli:")
        print("  https://github.com/GPUOpen-Tools/compressonator")
        return

    if not os.path.exists(CACHE_FILE_NAME):
        try:
            with open(CACHE_FILE_NAME, "w"):
                pass
        except OSError:
            print("> Coudn't create cache file")
            return
    with open(CACHE_FILE_NAME, "r+") as cacheFile:
        cache = {}

        try:
            name = None
            for line in cacheFile.read().splitlines():
                if name is None:
                    name = line
                else:
                    cache[name] = int(line)
                    name = None
        except:
            cache = {}

    for currentPath, folders, files in os.walk(DEFAULT_INPUT_FOLDER_NAME):
        for file in files:
            fullRelativeFilename = os.path.join(currentPath, file)

            filename = os.path.relpath(fullRelativeFilename, DEFAULT_INPUT_FOLDER_NAME)

            pathNoExt, ext = os.path.splitext(filename)
            isImg = ext.lower() in INPUT_EXTENSIONS

            if not isImg:
                continue

            isNew = filename not in cache

            lastModifTime = int(pathlib.Path(fullRelativeFilename).stat().st_mtime)
            isOutdated = filename in cache and lastModifTime != cache[filename]

            if isNew or isOutdated:

                isNormalMap = pathNoExt.lower().endswith("_n")

                sizeInBytes = os.stat(fullRelativeFilename).st_size
                minKilobytesForCompression = 25

                rawSizeInBytes = sizeInBytes
                tempFileToDelete = None
                inputFile = os.path.join(DEFAULT_INPUT_FOLDER_NAME, filename)

                try:
                    with Image.open(fullRelativeFilename) as im:
                        rawSizeInBytes = im.width * im.height * 4
                        if im.mode in ("P", "1", "L", "LA", "PA", "I", "F") or (ext.lower() == ".tga" and im.mode not in ("RGB", "RGBA")):
                            conv = im.convert("RGBA")
                            fd, tempPath = tempfile.mkstemp(suffix=".png")
                            os.close(fd)
                            conv.save(tempPath, format="PNG")
                            inputFile = tempPath
                            tempFileToDelete = tempPath
                except Exception:
                    pass

                if rawSizeInBytes < minKilobytesForCompression * 1024:
                    compressionFormat = "ARGB_8888"
                elif isNormalMap:
                    compressionFormat = "BC5"
                else:
                    compressionFormat = "BC7"

                print("> Converting " + filename + " (" + compressionFormat + ")")

                outputFile = os.path.join(DEFAULT_OUTPUT_FOLDER_NAME, pathNoExt + OUTPUT_EXTENSION)

                pathlib.Path(os.path.dirname(outputFile)).mkdir(parents=True, exist_ok=True)

                r = subprocess.run([
                    "compressonatorcli.exe",
                    "-fd", compressionFormat,
                    "-miplevels", "20",
                    "-EncodeWith", "GPU",
                    inputFile,
                    outputFile],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

                if tempFileToDelete and os.path.exists(tempFileToDelete):
                    try:
                        os.remove(tempFileToDelete)
                    except OSError:
                        pass

                if "Done Processing" in r.stdout:
                    cache[filename] = lastModifTime
                else:
                    print(r.stdout)

    with open(CACHE_FILE_NAME, "w") as cacheFile:
        for name, tm in cache.items():
            cacheFile.write(name + "\n" + str(tm) + "\n")


if __name__ == '__main__':
    main()
