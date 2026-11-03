# The user-scope task executor over the cleanup and placement fakes and FakeFileSystem: reading
# tasks, the allow-lists, running native commands, handlers and relocations, the report record,
# the relocation targets from the tray's polled volumes and disk health, and the owner's one-click
# decisions: a folder moved to the HDD archive (SafeRemove over FakeFileSystem) and an uninstaller
# started through a fake launcher that starts nothing.
myclean_add_test(user_tasks
  SOURCES
    archive_test.cpp
    drive_facts_test.cpp
    task_test.cpp
    uninstall_test.cpp
  DEPENDS
    myclean::user_tasks
    myclean::fakes)
