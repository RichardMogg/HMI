# Automatisches LittleFS-Upload nach jeder Firmware-Upload fuer s3_gateway
# Verhindert 'Datei nicht gefunden' nach Firmware-Updates
# Ab jetzt reicht ein einziges 'upload' - uploadfs laeuft automatisch danach.
Import("env")

def after_firmware_upload(source, target, env):
    print("\n[AutoFS] Lade LittleFS-Dateisystem automatisch hoch...")
    result = env.Execute(
        env.subst("$PYTHONEXE -m platformio run -e $PIOENV --target uploadfs")
    )
    if result != 0:
        print("[AutoFS] FEHLER: LittleFS-Upload fehlgeschlagen!")
    else:
        print("[AutoFS] LittleFS-Upload erfolgreich.")

env.AddPostAction("upload", after_firmware_upload)
