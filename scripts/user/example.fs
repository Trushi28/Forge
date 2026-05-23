// example.fs — Example ForgeScript plugin
//
// This script demonstrates ForgeScript by auto-formatting on save
// and providing a keyboard shortcut for wrapping text in bold markers.

on save {
    run "echo 'ForgeScript: file saved!' >> /tmp/forge.log"
    notify "saved and logged!"
}

on keypress Ctrl+B {
    wrap selection with "**" and "**"
}
