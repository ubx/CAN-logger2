Import("env")
import subprocess

app_name = env.GetProjectOption("custom_app_name", "firmware")
app_version = env.GetProjectOption("custom_app_version", "0.0.0")

try:
    git_rev = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"],
        stderr=subprocess.STDOUT
    ).decode().strip()
except Exception:
    git_rev = "nogit"

env.Replace(PROGNAME=f"{app_name}_v{app_version}_{git_rev}")
