import os
import zipfile
import shutil
import plistlib
import json
from pathlib import Path

def package():
    base_dir = Path(__file__).resolve().parent
    tmbundle_dir = base_dir / "Fool.tmbundle"
    syntaxes_dir = tmbundle_dir / "Syntaxes"
    json_path = syntaxes_dir / "Fool.tmLanguage.json"
    plist_path = syntaxes_dir / "Fool.tmLanguage"
    output_zip = base_dir / "Fool.tmbundle.zip"
    root_zip = base_dir.parent.parent / "Fool.tmbundle.zip"

    # 1. Convert JSON to XML plist format
    print(f"Generating Apple XML plist: {plist_path} ...")
    with open(json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    with open(plist_path, 'wb') as f:
        plistlib.dump(data, f)
    print("  Done converting plist.")

    # 2. Package into zip
    print(f"Bundling {tmbundle_dir} -> {output_zip} ...")
    with zipfile.ZipFile(output_zip, 'w', zipfile.ZIP_DEFLATED) as zf:
        for file in tmbundle_dir.rglob('*'):
            if file.is_file():
                arcname = file.relative_to(base_dir)
                zf.write(file, arcname)
                print(f"  Added {arcname}")
                
    shutil.copy2(output_zip, root_zip)
    print(f"Copied bundle zip to {root_zip}")
    print("Done!")

if __name__ == '__main__':
    package()
