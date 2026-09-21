Import("env")
import gzip
import os
import shutil
import time
from pathlib import Path

# Temp directory outside data folder
TEMP_BACKUP_DIR = Path(".pio/temp_www_backup")
LOCK_FILE = Path(".pio/temp_www_backup.lock")

def should_compress(source_file, gz_file):
    """Check if source file needs compression (newer than .gz or .gz doesn't exist)"""
    if not gz_file.exists():
        return True
    return source_file.stat().st_mtime > gz_file.stat().st_mtime

def compress_file(source_path):
    """Compress a single file with gzip"""
    gz_path = Path(str(source_path) + '.gz')
    
    if not should_compress(source_path, gz_path):
        return False
    
    try:
        with open(source_path, 'rb') as f_in:
            with gzip.open(gz_path, 'wb', compresslevel=9) as f_out:
                f_out.writelines(f_in)
        
        original_size = source_path.stat().st_size
        compressed_size = gz_path.stat().st_size
        ratio = (1 - compressed_size / original_size) * 100
        
        print(f"  + {source_path.name} -> {source_path.name}.gz ({original_size:,} -> {compressed_size:,} bytes, {ratio:.1f}% savings)")
        return True
    except Exception as e:
        print(f"  [error] compressing {source_path.name}: {e}")
        return False

def _get_selected_lang_code():
    import re
    
    # Get active build flags from environment
    # Import env from parent scope
    try:
        from SCons.Script import Import
        Import("env")
        
        active_defines = set()
        cpp_defines = env.get("CPPDEFINES", [])
        for item in cpp_defines:
            if isinstance(item, str):
                active_defines.add(item.split("=")[0])
            elif isinstance(item, tuple) and len(item) >= 1:
                active_defines.add(item[0])
        
        # Fallback: derive from environment name
        env_name = env.get("PIOENV", "")
        if env_name:
            active_defines.add(env_name.upper())
    except:
        active_defines = set()
    
    # Helper to find DSP_LANGUAGE or WEBUI_LANGUAGE flag pattern: #define DSP_LANGUAGE_lt_LT or #define WEBUI_LANGUAGE_lt_LT
    def _find_language_flag(path, prefix="DSP_LANGUAGE"):
        try:
            with open(path, "r", encoding="utf-8") as fh:
                in_active_block = True
                if_depth = 0
                block_stack = []
                
                # Read file and handle line continuations
                lines = []
                temp_line = ""
                for raw_line in fh:
                    if raw_line.rstrip().endswith('\\'):
                        temp_line += raw_line.rstrip()[:-1] + " "  # Remove \ and add space
                    else:
                        lines.append(temp_line + raw_line)
                        temp_line = ""
                
                for ln in lines:
                    ln_stripped = ln.strip()
                    
                    # Track #if defined(...) blocks (simple or OR'd)
                    if re.match(r"#\s*if\s+", ln_stripped):
                        # Extract all defined(FLAG) patterns
                        flags = re.findall(r"defined\((\w+)\)", ln_stripped)
                        if flags:
                            # Block is active if ANY flag matches
                            is_active = any(flag in active_defines for flag in flags)
                            if_depth += 1
                            block_stack.append((if_depth, is_active))
                            in_active_block = is_active
                            continue
                        else:
                            # Complex #if without defined() - treat as inactive
                            if_depth += 1
                            block_stack.append((if_depth, False))
                            in_active_block = False
                            continue
                        
                    # Track #ifdef
                    ifdef_match = re.match(r"#\s*ifdef\s+(\w+)", ln_stripped)
                    if ifdef_match:
                        flag = ifdef_match.group(1)
                        is_active = flag in active_defines
                        if_depth += 1
                        block_stack.append((if_depth, is_active))
                        in_active_block = is_active
                        continue
                    
                    # Track #elif defined(...)
                    if re.match(r"#\s*elif\s+", ln_stripped):
                        flags = re.findall(r"defined\((\w+)\)", ln_stripped)
                        if flags and block_stack:
                            # Pop previous block at same depth, evaluate new condition
                            block_stack.pop()
                            if_depth = max(1, if_depth)
                            is_active = any(flag in active_defines for flag in flags)
                            block_stack.append((if_depth, is_active))
                            in_active_block = is_active
                            continue
                    
                    # Track #else
                    if re.match(r"#\s*else", ln_stripped):
                        if block_stack:
                            # Pop previous block, invert its condition
                            old_depth, old_active = block_stack.pop()
                            is_active = not old_active
                            block_stack.append((old_depth, is_active))
                            in_active_block = is_active
                            continue
                        
                    # Track #endif
                    if re.match(r"#\s*endif", ln_stripped):
                        if block_stack:
                            block_stack.pop()
                            if_depth = max(0, if_depth - 1)
                            in_active_block = block_stack[-1][1] if block_stack else True
                        continue
                    
                    # Look for DSP_LANGUAGE_xx_XX or WEBUI_LANGUAGE_xx_XX pattern
                    if in_active_block:
                        m = re.match(r'\s*#\s*define\s+' + prefix + r'_([a-zA-Z]{2}_[a-zA-Z]{2})', ln)
                        if m:
                            return m.group(1)
        except Exception:
            pass
        return None
    
    # Look for WEBUI_LANGUAGE_xx_XX first, then fall back to DSP_LANGUAGE_xx_XX
    lang_source = None
    macro_used = None
    for candidate in ("myoptions.h",):
        p = Path(candidate)
        if not p.exists():
            continue
        # Try WEBUI_LANGUAGE flag first
        result = _find_language_flag(str(p), "WEBUI_LANGUAGE")
        if result:
            lang_source = candidate
            macro_used = "WEBUI_LANGUAGE"
            return result, lang_source, macro_used
        # Fall back to DSP_LANGUAGE flag
        result = _find_language_flag(str(p), "DSP_LANGUAGE")
        if result:
            lang_source = candidate
            macro_used = "DSP_LANGUAGE"
            return result, lang_source, macro_used

    return "en_US", None, None  # fallback default


def deploy_locale_json(source, target, env):
    """Copy the chosen WebUI language JSON into the LittleFS source tree.

    The language code is derived via :pyfunc:`_get_selected_lang_code`, which
    reads ``WEBUI_LOCALE`` if defined or falls back to
    ``DSP_LANGUAGE``.  If English (``en_US``), no file is copied since the
    JS layer uses built-in defaults from ``language.json``.  Otherwise, the
    selected locale JSON is copied from ``src/locale/webui/{code}.json`` to
    ``data/www/lang_{code}.json``.  Any stale ``lang_*.json`` files are
    removed.
    """
    lang_code, lang_source, macro_used = _get_selected_lang_code()
    
    # Debug output for language detection
    if lang_source:
        source_name = os.path.basename(lang_source) if lang_source else "unknown"
        print(f"  [locale] Detected {macro_used}={lang_code} from {source_name}")
    else:
        print(f"  [locale] No language defined, using default {lang_code}")

    www_dir = Path("data/www")
    locale_src = Path("src/locale/webui")

    # Remove stale locale files (pattern: xx_XX.json where x is a letter)
    import re
    locale_pattern = re.compile(r'^[a-z]{2}_[A-Z]{2}\.json$')
    for old_file in www_dir.glob("*.json"):
        if locale_pattern.match(old_file.name):
            old_file.unlink()
            print(f"  [locale] removed stale {old_file.name}")

    # Copy the selected language file with its original name
    json_file = locale_src / f"{lang_code}.json"
    
    # Read HARDCODED_WEBUI_LOCALE from locale.h
    hardcoded_locale = "en_US"  # default fallback
    locale_h_path = Path("src/core/locale.h")
    if locale_h_path.exists():
        with open(locale_h_path, 'r', encoding='utf-8') as f:
            for line in f:
                match = re.match(r'\s*#\s*define\s+HARDCODED_WEBUI_LOCALE\s+"([^"]+)"', line)
                if match:
                    hardcoded_locale = match.group(1)
                    break
    
    # Skip if selected language matches hardcoded locale (hardcoded HTML text will be used)
    if lang_code == hardcoded_locale:
        print(f"  [locale] No translation file needed ({lang_code} uses hardcoded HTML text)")
        return
    
    if not json_file.exists():
        print(f"  [locale] Translation file not found: {json_file}")
        print(f"  [locale] Skipping locale deployment")
        return
    
    dest = www_dir / f"{lang_code}.json"
    shutil.copy2(str(json_file), str(dest))
    print(f"  [locale] {json_file} -> {dest}")
    print(f"  [locale] WebUI locale deployed for {lang_code}")


def acquire_lock():
    """Wait for and acquire the build lock file to prevent concurrent builds"""
    max_wait = 300  # Maximum wait time in seconds
    wait_interval = 1  # Check every 1 second
    elapsed = 0
    
    while LOCK_FILE.exists():
        if elapsed == 0:
            print("\n" + "="*70)
            print("WAITING: Another build is in progress. Waiting up to 3 minutes...")
            print("\t- First, check the www files for problems")
            print(f"\t- Then, override by deleting {LOCK_FILE}")
            print("="*70)
        time.sleep(wait_interval)
        elapsed += wait_interval
        if elapsed >= max_wait:
            # ABORT the build - do NOT proceed if another build is still running
            print("\n" + "="*70)
            print(f"ERROR: Lock file timeout after {max_wait}s")
            print(f"Another build process appears stuck or is still running.")
            print(f"")
            print(f"If you're certain no other build is running, manually delete {LOCK_FILE}")
            print("="*70)
            raise SystemExit(1)  # Abort the build completely
    
    # Extra safety delay if we had to wait
    if elapsed > 0:
        print("Previous build finished, waiting 2s for safety...")
        time.sleep(2)
    
    # Create lock file
    LOCK_FILE.parent.mkdir(parents=True, exist_ok=True)
    LOCK_FILE.touch()
    print(f"\nBuild lock acquired: {LOCK_FILE}")

def compress_and_hide_originals(source, target, env):
    """Compress web files and temporarily move originals so only .gz files are in LittleFS"""
    print("\n" + "="*70)
    print("PRE-BUILD: Compressing web files for LittleFS...")
    print("="*70)

    # Stage locale JSON files before compression pass
    print("\nDeploying locale JSON file:")
    deploy_locale_json(source, target, env)
    
    data_dir = Path("data/www")
    if not data_dir.exists():
        print(f"Warning: {data_dir} does not exist, skipping compression")
        return
    
    # Clear and recreate temp backup directory to ensure clean state
    if TEMP_BACKUP_DIR.exists():
        shutil.rmtree(TEMP_BACKUP_DIR)
    TEMP_BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    
    # Files to exclude from compression (by filename, any directory)
    exclude = ["rb_srvrs.json"]
    # Subdirectories to exclude from compression — files are kept as plain files in LittleFS
    # (avoids ESPAsyncWebServer gzip+subdirectory edge cases for small files)
    exclude_dirs = []
    
    compressed_count = 0
    skipped_count = 0
    
    print("\nGzipping files:")
    # First pass: compress all files recursively
    for file_path in sorted(data_dir.rglob("*")):
        if not file_path.is_file():
            continue
        rel = file_path.relative_to(data_dir)
        # Exclude files in excluded subdirectories
        if any(part in exclude_dirs for part in rel.parts[:-1]):
            print(f"  [skip] {rel} (excluded dir)")
            continue
        if file_path.name in exclude:
            print(f"  [skip] {rel} (excluded)")
            continue

        # Skip files that are already gzipped (avoid creating .gz.gz entries)
        if file_path.name.endswith('.gz'):
            print(f"  [skip] {file_path.relative_to(data_dir)} (already gzipped)")
            skipped_count += 1
            continue
        
        if compress_file(file_path):
            compressed_count += 1
        else:
            skipped_count += 1
    
    print("-"*70)
    print(f"Compressed: {compressed_count} files | Skipped: {skipped_count} files (already up-to-date)")
    print("="*70)
    
    # Second pass: move originals outside data directory (preserve relative subpath in backup)
    print("\nMoving original files out of data/www (only .gz and excluded files will be in LittleFS):")
    hidden_count = 0
    moved_names = []
    for file_path in sorted(data_dir.rglob("*")):
        if not file_path.is_file():
            continue
        rel = file_path.relative_to(data_dir)
        if any(part in exclude_dirs for part in rel.parts[:-1]):
            continue
        if file_path.name in exclude:
            continue
        if file_path.name.endswith('.gz'):
            continue
        
        gz_path = Path(str(file_path) + '.gz')
        if gz_path.exists():
            rel = file_path.relative_to(data_dir)
            backup_path = TEMP_BACKUP_DIR / rel
            backup_path.parent.mkdir(parents=True, exist_ok=True)
            if backup_path.exists():
                backup_path.unlink()
            shutil.move(str(file_path), str(backup_path))
            hidden_count += 1
            moved_names.append(f"-> {rel}")
    
    print(" ".join(moved_names))
    print(f"Moved {hidden_count} original files to {TEMP_BACKUP_DIR}")
    print(f"LittleFS will contain ONLY .gz files (and excluded files)")
    print("="*70 + "\n")

# Detect if we're doing a filesystem operation
import sys
any_fs_target = any(t in sys.argv for t in ["uploadfs", "buildfs", "--target"])

if any_fs_target:
    # Acquire before doing anything
    acquire_lock()
    
    # Delete cached littlefs.bin to force rebuild
    littlefs_bin = Path(env.subst("$BUILD_DIR")) / "littlefs.bin"
    if littlefs_bin.exists():
        print("\n" + "="*70)
        print("INIT: Deleting cached littlefs.bin")
        print("="*70)
        littlefs_bin.unlink()
        print("  → Deleted littlefs.bin - will rebuild with compression")
        print("="*70 + "\n")
    
    # Run compression now at init time
    compress_and_hide_originals(None, None, env)

# Note: NOT using AddPreAction here because we run compression at init time instead
# This ensures compression always runs for filesystem operations
