package org.wwhdrecomp.app;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

/**
 * The fan translation into Brazilian Portuguese (Triforce-Heroes) as a mod: the user chooses the
 * translation's folder (the Cemu graphic pack or the console version, as they are distributed); its
 * message packs (content/Common/Pack/permanent_2d_*.pack) are copied to files/mods/ptbr/content/...
 * and, while the mod is on, the game reads them instead of its own (runtime/src/hle/fs.cpp, content
 * overlay). The translation replaces the English text, so the game language is set to English. No
 * file of the translation is part of the app.
 */
final class Translation {
    private Translation() {}

    static File dir(Context c, File base) { return new File(base, "mods/ptbr"); }

    static File packDir(File dir) { return new File(dir, "content/Common/Pack"); }

    static boolean installed(File dir) {
        File[] packs = packDir(dir).listFiles((d, n) -> n.toLowerCase(Locale.ROOT).startsWith("permanent_2d_") && n.endsWith(".pack"));
        return packs != null && packs.length > 0;
    }

    /** the translation's name and version from its rules.txt, or "" */
    static String label(File dir) {
        File f = new File(dir, "label.txt");
        if (!f.exists()) return "";
        try {
            return new String(java.nio.file.Files.readAllBytes(f.toPath()), java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    /**
     * Copies the message packs found anywhere under `tree` (a folder the user chose) into `dir`.
     * Returns null, or why not.
     */
    static String install(Context c, Uri tree, File dir) {
        ContentResolver cr = c.getContentResolver();
        File packs = packDir(dir);
        File tmp = new File(dir.getParentFile(), "ptbr.part");
        Backup.deleteTree(tmp);
        File tmpPacks = packDir(tmp);
        if (!tmpPacks.mkdirs()) return "cannot create " + tmpPacks;
        int[] copied = {0};
        String[] label = {""};
        try {
            walk(cr, tree, DocumentsContract.getTreeDocumentId(tree), 0, (name, uri) -> {
                String lower = name.toLowerCase(Locale.ROOT);
                if (lower.startsWith("permanent_2d_") && lower.endsWith(".pack")) {
                    File out = new File(tmpPacks, name);
                    if (out.exists()) return;  // the first one found (both versions hold the same files)
                    copy(cr, uri, out);
                    copied[0]++;
                } else if (lower.equals("rules.txt") && label[0].isEmpty()) {
                    label[0] = rulesLabel(cr, uri);
                }
            });
        } catch (IOException e) {
            Backup.deleteTree(tmp);
            return e.getMessage();
        }
        if (copied[0] == 0) {
            Backup.deleteTree(tmp);
            return c.getString(R.string.ptbr_not_found);
        }
        try (OutputStream o = new FileOutputStream(new File(tmp, "label.txt"))) {
            o.write(label[0].getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
        Backup.deleteTree(dir);
        if (!tmp.renameTo(dir)) return "cannot move the translation into place";
        return null;
    }

    static void remove(File dir) { Backup.deleteTree(dir); }

    /**
     * A personal build can carry the translation in its assets (ptbr/..., gradle -PwwhdBundleAssets):
     * installed on the first start, and again when the build carries another version (label.txt).
     */
    static void installBundled(Context c, File dir) {
        android.content.res.AssetManager am = c.getAssets();
        try {
            String[] packs = am.list("ptbr/content/Common/Pack");
            if (packs == null || packs.length == 0) return;
            String label = "";
            try (InputStream in = am.open("ptbr/label.txt")) {
                label = new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8).trim();
            } catch (IOException ignored) {
            }
            if (installed(dir) && label.equals(label(dir))) return;
            File tmp = new File(dir.getParentFile(), "ptbr.part");
            Backup.deleteTree(tmp);
            File out = packDir(tmp);
            if (!out.mkdirs()) return;
            for (String p : packs)
                try (InputStream in = am.open("ptbr/content/Common/Pack/" + p); OutputStream o = new FileOutputStream(new File(out, p))) {
                    byte[] buf = new byte[1 << 16];
                    for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
                }
            // identical packs are carried once: "copy=original" lines
            try (InputStream in = am.open("ptbr/aliases.txt")) {
                for (String line : new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8).split("\n")) {
                    int eq = line.indexOf('=');
                    if (eq <= 0) continue;
                    File from = new File(out, line.substring(eq + 1).trim()), to = new File(out, line.substring(0, eq).trim());
                    if (from.exists()) java.nio.file.Files.copy(from.toPath(), to.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                }
            } catch (IOException ignored) {
            }
            try (OutputStream o = new FileOutputStream(new File(tmp, "label.txt"))) {
                o.write(label.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            Backup.deleteTree(dir);
            if (!tmp.renameTo(dir)) Backup.deleteTree(tmp);
        } catch (IOException ignored) {
        }
    }

    private interface Visitor { void file(String name, Uri uri) throws IOException; }

    private static void walk(ContentResolver cr, Uri tree, String docId, int depth, Visitor v) throws IOException {
        if (depth > 12) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (Cursor cur = cr.query(children, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (cur != null && cur.moveToNext()) {
                String id = cur.getString(0), name = cur.getString(1), mime = cur.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) walk(cr, tree, id, depth + 1, v);
                else v.file(name, DocumentsContract.buildDocumentUriUsingTree(tree, id));
            }
        }
    }

    private static void copy(ContentResolver cr, Uri from, File to) throws IOException {
        try (InputStream in = cr.openInputStream(from); OutputStream out = new FileOutputStream(to)) {
            if (in == null) throw new IOException("cannot read " + from);
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        }
    }

    // "path = ".../Tradução para PT-BR v2025.08.05 by Triforce-Heroes (100.0%)"": its last part
    private static String rulesLabel(ContentResolver cr, Uri uri) {
        try (InputStream in = cr.openInputStream(uri)) {
            if (in == null) return "";
            String s = new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8);
            for (String line : s.split("\n")) {
                line = line.trim();
                if (line.startsWith("path")) {
                    int q1 = line.indexOf('"'), q2 = line.lastIndexOf('"');
                    if (q1 >= 0 && q2 > q1) {
                        String p = line.substring(q1 + 1, q2);
                        return p.substring(p.lastIndexOf('/') + 1);
                    }
                }
            }
        } catch (IOException ignored) {
        }
        return "";
    }
}
