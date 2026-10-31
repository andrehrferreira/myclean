# The user-scope task executor over the cleanup and placement fakes and FakeFileSystem: reading
# tasks, the allow-lists, running native commands, handlers and relocations, the report record,
# and the relocation targets from the tray's polled volumes and disk health.
myclean_add_test(user_tasks
  SOURCES
    drive_facts_test.cpp
    task_test.cpp
  DEPENDS
    myclean::user_tasks
    myclean::fakes)
