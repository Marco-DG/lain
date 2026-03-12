const fs = require('fs');

let content = fs.readFileSync('specs/Introducing Austral_ A Systems Language with Linear Types and Capabilities.html', 'utf-8');

// Generic text replacements
content = content.replace(/Austral/g, 'Lain');
content = content.replace(/austral/g, 'lain');

const replacements = [
    [`-- \`Pos\` is declared to be linear, even though it
-- only contains free types.
record Pos: Linear is
    x: Int32;
    y: Int32;
end;`,
        `// \`Pos\` is declared to be linear by containing at least one \`mov\` field.
type Pos {
    mov _linear bool
    x int
    y int
}`],

    [`record Example: Linear is
    a: A;
    b: B;
    c: Pair[T, A];
end;`,
        `type Example {
    a A
    b B
    c Pair_T_A
}`],

    [`begin
    let x: L := f();
end;`,
        `{
    var x = f()
}`],

    [`begin
    f();
end;`,
        `{
    f()
}`],

    [`begin
    let x: L := f();
    g(x);
    h(x);
end;`,
        `{
    var x = f()
    g(mov x)
    h(mov x)
}`],

    [`begin
    let x: L := f();
    g(x);
end;`,
        `{
    var x = f()
    g(mov x)
}`],

    [`begin
    let x: L := f();
    if cond() then
        g(x);
    else
        -- Do nothing.
        skip;
    end if;
end;`,
        `{
    var x = f()
    if cond() {
        g(mov x)
    } else {
        // Do nothing.
    }
}`],

    [`begin
    let x: L := f();
    if cond() then
        g(x);
    else
        h(x);
    end if;
end;`,
        `{
    var x = f()
    if cond() {
        g(mov x)
    } else {
        h(mov x)
    }
}`],

    [`begin
    let x: L := f();
    while cond() do
        g(x);
    end while;
end;`,
        `{
    var x = f()
    while cond() {
        g(mov x)
    }
}`],

    [`type File
File openFile(String path)
File writeString(File file, String content)
void closeFile(File file)`,
        `type File
func openFile(path string) File
func writeString(mov file File, content string) File
func closeFile(mov file File)`],

    [`type Db
Db connect(String host)
Rows query(Db db, String query)
void close(Db db)`,
        `type Db
func connect(host string) Db
func query(mov db Db, query string) Rows
func close(mov db Db)`],

    [`type Pointer&lt;T&gt;
Pointer&lt;T&gt; allocate(T value)
T load(Pointer&lt;T&gt; ptr)
void store(Pointer&lt;T&gt; ptr, T value)
void free(Pointer&lt;T&gt; ptr)`,
        `type Pointer_T
func allocate(value T) Pointer_T
func load(borrow ptr Pointer_T) T
func store(mut borrow ptr Pointer_T, value T)
func free(mov ptr Pointer_T)`],

    [` let file = openFile("hello.txt");
 writeString(file, "Hello, world!");
 // Forgot to close`,
        ` var file = openFile("hello.txt")
 writeString(mov file, "Hello, world!")
 // Forgot to close`],

    [`closeFile(file);
writeString(file, "Goodbye, world!");`,
        `closeFile(mov file)
writeString(mov file, "Goodbye, world!")`],

    [`closeFile(file);
closeFile(file);`,
        `closeFile(mov file)
closeFile(mov file)`],

    [`module Files is
    type File : Linear;
    function openFile(path: String): File;
    function writeString(file: File, content: String): File;
    function closeFile(file: File): Unit;
end module.`,
        `type File {
    mov handle *int
}
func openFile(path string) File
func writeString(mov file File, content string) File
func closeFile(mov file File)`],

    [`let file: File := openFile("test.txt");
-- Do nothing.`,
        `var file = openFile("test.txt")
// Do nothing.`],

    [`let file: File := openFile("test.txt");
writeString(file, "Hello, world!");`,
        `var file = openFile("test.txt")
writeString(mov file, "Hello, world!")`],

    [`let file: File := openFile("test.txt");
closeFile(file);
closeFile(file);`,
        `var file = openFile("test.txt")
closeFile(mov file)
closeFile(mov file)`],

    [`let file: File := openFile("test.txt");
closeFile(file);
let file2: File := writeString(file, "Doing some mischief.");`,
        `var file = openFile("test.txt")
closeFile(mov file)
var file2 = writeString(mov file, "Doing some mischief.")`],

    [`let f: File := openFile("test.txt");
let f1: File := writeString(f, "First line");
let f2: File := writeString(f1, "Another line");
...
let f15: File := writeString(f14, "Last line");
closeFile(f15);`,
        `var f = openFile("test.txt")
var f1 = writeString(mov f, "First line")
var f2 = writeString(mov f1, "Another line")
// ...
var f15 = writeString(mov f14, "Last line")
closeFile(mov f15)`],

    [`module Database is
    type Db: Linear;
    function connect(host: String): Db;
    function query(db: Db, query: String): Pair[Db, Rows];
    function close(db: Db): Unit;
end module.`,
        `type Db {
    mov handle *int
}
type DbRowsPair {
    db Db
    rows Rows
}
func connect(host string) Db
func query(mov db Db, query string) DbRowsPair
func close(mov db Db)`],

    [`let db: Db := connect("localhost");
-- Do nothing.`,
        `var db = connect("localhost")
// Do nothing.`],

    [`let db: Db := connect("localhost");
close(db);
close(db); -- error: \`db\` consumed again.`,
        `var db = connect("localhost")
close(mov db)
close(mov db) // error: \`db\` consumed again.`],

    [`let db: Db := connect("localhost");
close(db);
-- The below is tuple destructuring notation.
let { first as db1: Db, second: Rows } := query(db, "SELECT ...");
close(db); -- error: \`db\` consumed again.
-- another error: \`db1\` never consumed.`,
        `var db = connect("localhost")
close(mov db)

var res = query(mov db, "SELECT ...")
var db1 = mov res.db
var rows = res.rows

close(mov db) // error: \`db\` consumed again.
// another error: \`db1\` never consumed.`],

    [`let db: Db := connect("localhost");
let { first as db1: Db, second: Rows } = query(db, "SELECT ...");
// Iterate over the rows or some such.
close(db1);`,
        `var db = connect("localhost")
var res = query(mov db, "SELECT ...")
var db1 = mov res.db
// Iterate over the rows or some such.
close(mov db1)`],

    [`module Files is
    -- File and directory paths.
    type Path: Linear;
    -- Creating and disposing of paths.
    function Make_Path(value: String): Path;
    function Dispose_Path(path: Path): Unit;
    -- Reading and writing.
    generic [R: Region]
    function Read_File(path: &amp;[Path, R]): String;
    generic [R: Region]
    function Write_File(path: &amp;![Path, R], content: String): Unit;
end module.`,
        `// File and directory paths.
type Path {
    mov handle *int
}

// Creating and disposing of paths.
func Make_Path(value string) Path
func Dispose_Path(mov path Path)

// Reading and writing.
func Read_File(borrow path Path) string
func Write_File(mut borrow path Path, content string)`],

    [`let p: Path := Parse_Path(Make_String("/etc/passwd"));
let secrets: String := Read_File(&amp;p);
-- Send this over the network, using an equally capability-insecure network
-- API.
uploadToCompromisedServer(secrets);`,
        `var p = Parse_Path("/etc/passwd")
var secrets = Read_File(borrow p)
// Send this over the network, using an equally capability-insecure network API.
uploadToCompromisedServer(secrets)`],

    [`module Files is
    type Path: Linear;
    -- The filesystem access capability.
    type Filesystem: Linear;
    -- Given a read reference to the filesystem access capability,
    -- get the root directory.
    generic [R: Region]
    function Get_Root(fs: &amp;[Filesystem, R]): Path;
    -- Given a directory path, append a directory or
    -- file name at the end.
    function Append(path: Path, name: String): Path;
    -- Reading and writing.
    generic [R: Region]
    function Read_File(path: &amp;[Path, R]): String;
    generic [R: Region]
    function Write_File(path: &amp;[Path, R], content: String): Unit;
end module.`,
        `type Path { mov handle *int }
// The filesystem access capability.
type Filesystem { mov capability *int }

// Given a read reference to the filesystem access capability,
// get the root directory.
func Get_Root(borrow fs Filesystem) Path

// Given a directory path, append a directory or
// file name at the end.
func Append(borrow path Path, name string) Path

// Reading and writing.
func Read_File(borrow path Path) string
func Write_File(mut borrow path Path, content string)`],

    [`-- Acquire the filesystem capability from a reference
-- to the root capability.
generic [R: Region]
function Get_Filesystem(root: &amp;[Root_Capability, R]): Filesystem;

-- Relinquish the filesystem capability.
function Release_Filesystem(fs: Filesystem): Unit;`,
        `// Acquire the filesystem capability from a reference
// to the root capability.
func Get_Filesystem(borrow root Root_Capability) Filesystem

// Relinquish the filesystem capability.
func Release_Filesystem(mov fs Filesystem)`],

    [`import Files (
    Filesystem,
    Get_Filesystem,
    Path,
    Get_Root,
    Append,
    Relase_Filesystem,
);
import Dependency (
    Do_Something
);

function Main(root: Root_Capability): Root_Capability is
    -- Acquire a filesystem capability.
    let fs: Filesystem := Get_Filesystem(&amp;root);
    -- Get the root directory.
    let r: Path := Get_Root(&amp;fs);
    -- Get the path to the \`/var\` directory.
    let p: Path := Append(r, "var");
    -- Do something with the path to the \`/var\` directory, confident that nothing
    -- this dependency does can go outside \`/var\`.
    Do_Something(p);
    -- Afterwards, relinquish the filesystem capability.
    Release_Filesystem(fs);
    -- Finally, end the program by returning the root capability.
    return root;
end;`,
        `func Main(mov root Root_Capability) Root_Capability {
    // Acquire a filesystem capability.
    var fs = Get_Filesystem(borrow root)
    // Get the root directory.
    var r = Get_Root(borrow fs)
    // Get the path to the \`/var\` directory.
    var p = Append(borrow r, "var")
    
    // Do something with the path to the \`/var\` directory, confident that nothing
    // this dependency does can go outside \`/var\`.
    Do_Something(mov p)
    
    // Afterwards, relinquish the filesystem capability.
    Release_Filesystem(mov fs)
    
    // Finally, end the program by returning the root capability.
    return mov root
}`],

    [`function main(): ExitCode is
    printLn("Hello, world!");
    return ExitSuccess();
end;`,
        `proc main() int {
    printLn("Hello, world!")
    return 0
}`],

    [`https://borretti.me/assets/card/introducing-austral.jpg`,
        `https://borretti.me/assets/card/introducing-lain.jpg`]
];

replacements.sort((a, b) => b[0].length - a[0].length);

for (let [orig, rep] of replacements) {
    if (!content.includes(orig) && !content.includes(orig.split('&').join('&amp;'))) {
        console.log("NOT FOUND: " + orig.substring(0, 30));
    }
    content = content.split(orig).join(rep);
    content = content.split(orig.split('&').join('&amp;')).join(rep);
}

fs.writeFileSync('specs/Introducing Lain_ A Systems Language with Linear Types and Capabilities.html', content);
console.log("Done");
