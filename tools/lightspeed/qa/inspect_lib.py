import sqlite3, sys
cfg = sys.argv[1] if len(sys.argv) > 1 else r"C:/lightspeed/qa/lrimport-cfg"
db = sqlite3.connect("file:%s/library.db?mode=ro" % cfg.replace("\\", "/"), uri=True)
db.execute("attach database 'file:%s/data.db?mode=ro' as data" % cfg.replace("\\", "/"))
c = db.cursor()
print("id | file | ver | stars | rejected | labels | tags | group | history rows")
for r in c.execute("""select i.id, i.filename, i.version, i.flags & 7, (i.flags & 8)>0,
       (select group_concat(color) from color_labels cl where cl.imgid=i.id),
       (select group_concat(t.name,'; ') from main.tagged_images ti join data.tags t on t.id=ti.tagid
         where ti.imgid=i.id and t.name not like 'darktable|format%' and t.name != 'darktable|changed'),
       i.group_id, (select count(*) from history h where h.imgid=i.id), i.history_end
       from images i order by i.id"""):
    print(r)
print("history (non default ops):")
for r in c.execute("""select imgid, num, operation, multi_priority, multi_name from history
       where multi_name not like '_builtin%' and operation not in ('rawprepare','demosaic','colorin','colorout','gamma','temperature','highlights')
       order by imgid, num"""):
    print("  ", r)
