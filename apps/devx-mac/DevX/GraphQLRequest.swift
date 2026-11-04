import Foundation

/// One GraphQL operation a request carried, read from the request as it was
/// sent. Every field is text from the request; nothing is filled in.
struct GraphQLOperation: Equatable {
    /// "query", "mutation" or "subscription", read from the document. Empty
    /// when no document was sent (a persisted query sends only its hash).
    var kind: String = ""
    /// `operationName`, or the name in the document. Empty when anonymous.
    var name: String = ""
    /// The document as sent, with its own line breaks.
    var document: String = ""
    /// `variables` as the raw JSON text that was sent. Empty when none.
    var variables: String = ""
    /// The sha256 of an automatic persisted query, when one was sent.
    var persistedHash: String = ""

    /// "mutation AddToCart", "query (anonymous)", "persisted GetMenu".
    var label: String {
        let what = kind.isEmpty ? (persistedHash.isEmpty ? "graphql" : "persisted") : kind
        return what + " " + (name.isEmpty ? "(anonymous)" : name)
    }
}

/// GraphQL read out of a captured request, whatever way it was sent:
///
/// - a POST of `{"query", "operationName", "variables", "extensions"}`
///   (Apollo, urql, graphql-request, Relay, plain fetch or axios);
/// - a batch: a JSON array of those;
/// - a GET with the same fields in the query string, including Apollo's
///   automatic persisted queries, which send only a hash;
/// - a multipart upload (the GraphQL multipart request spec), whose
///   `operations` part holds the JSON;
/// - `Content-Type: application/graphql`, where the body is the document.
///
/// A request that is none of these is not GraphQL, and the result is empty:
/// nothing is called GraphQL on a guess.
enum GraphQLRequest {
    static func operations(_ row: JSON) -> [GraphQLOperation] {
        var contentType = ""
        if case .object(let headers) = row["request_headers"] {
            for (k, v) in headers where k.lowercased() == "content-type" { contentType = v.text }
        }
        let body: String? = row.isAbsent("request_body") ? nil : row["request_body"].text
        return operations(method: row["method"].text, url: row["url"].text,
                          body: body, contentType: contentType)
    }

    static func operations(method: String, url: String, body: String?,
                           contentType: String) -> [GraphQLOperation] {
        let type = contentType.lowercased()
        if let body, !body.isEmpty {
            if type.hasPrefix("application/graphql") && !type.contains("json") {
                return [fromDocument(body, name: "")]
            }
            if type.hasPrefix("multipart/") || body.hasPrefix("--") {
                if let ops = multipartOperations(body) { return fromJSON(ops) }
                return []
            }
            return fromJSON(body)
        }
        if method == "GET" { return fromQueryString(url) }
        return []
    }

    /// The messages in the response's `errors`: a GraphQL server reports a
    /// failed operation inside a 200, so the status alone says nothing.
    /// Collected over a batch. nil when the body is not a GraphQL result.
    static func errorMessages(inResponse body: String) -> [String]? {
        guard let top = JSONSpans.top(body) else { return nil }
        let results: [Substring] = top.isArray ? top.items.map(\.raw) : [Substring(body)]
        var messages: [String] = []
        var sawResult = false
        for r in results {
            guard let obj = JSONSpans.top(String(r)), !obj.isArray else { continue }
            let keys = Set(obj.items.compactMap(\.key))
            guard keys.contains("data") || keys.contains("errors") else { continue }
            sawResult = true
            guard let errors = obj.items.first(where: { $0.key == "errors" }),
                  let list = JSONSpans.top(String(errors.raw)), list.isArray else { continue }
            for e in list.items {
                let message = JSONSpans.top(String(e.raw))?.items
                    .first(where: { $0.key == "message" }).flatMap { JSONSpans.string($0.raw) }
                messages.append(message ?? String(e.raw))
            }
        }
        return sawResult ? messages : nil
    }

    // MARK: - the ways a request carries it

    private static func fromJSON(_ text: String) -> [GraphQLOperation] {
        guard let top = JSONSpans.top(text) else { return [] }
        if top.isArray {
            let ops = top.items.compactMap { item in
                JSONSpans.top(String(item.raw)).flatMap { $0.isArray ? nil : operation($0) }
            }
            // A batch only when every entry is an operation: an array of
            // something else is not GraphQL.
            return ops.count == top.items.count ? ops : []
        }
        return operation(top).map { [$0] } ?? []
    }

    private static func operation(_ obj: JSONSpans.Top) -> GraphQLOperation? {
        func raw(_ key: String) -> Substring? { obj.items.first(where: { $0.key == key })?.raw }
        let document = raw("query").flatMap(JSONSpans.string) ?? ""
        let name = raw("operationName").flatMap(JSONSpans.string) ?? ""
        let hash = raw("extensions").flatMap(persistedHash) ?? ""
        guard looksLikeDocument(document) || !hash.isEmpty else { return nil }
        var op = document.isEmpty ? GraphQLOperation(name: name) : fromDocument(document, name: name)
        if let v = raw("variables"), v != "null" { op.variables = String(v) }
        op.persistedHash = hash
        return op
    }

    private static func fromQueryString(_ url: String) -> [GraphQLOperation] {
        guard let items = URLComponents(string: url)?.queryItems else { return [] }
        func value(_ key: String) -> String? { items.first(where: { $0.name == key })?.value }
        let document = value("query") ?? ""
        let hash = value("extensions").flatMap { persistedHash(Substring($0)) } ?? ""
        guard looksLikeDocument(document) || !hash.isEmpty else { return [] }
        let name = value("operationName") ?? ""
        var op = document.isEmpty ? GraphQLOperation(name: name) : fromDocument(document, name: name)
        if let v = value("variables"), v != "null" { op.variables = v }
        op.persistedHash = hash
        return [op]
    }

    /// The `operations` part of a multipart upload.
    private static func multipartOperations(_ body: String) -> String? {
        guard let field = body.range(of: "name=\"operations\""),
              let start = body.range(of: "\r\n\r\n", range: field.upperBound..<body.endIndex)
                ?? body.range(of: "\n\n", range: field.upperBound..<body.endIndex)
        else { return nil }
        let rest = body[start.upperBound...]
        let end = rest.range(of: "\r\n--")?.lowerBound ?? rest.range(of: "\n--")?.lowerBound ?? rest.endIndex
        return String(rest[..<end])
    }

    private static func persistedHash(_ extensions: Substring) -> String? {
        guard let ext = JSONSpans.top(String(extensions)), !ext.isArray,
              let pq = ext.items.first(where: { $0.key == "persistedQuery" }),
              let fields = JSONSpans.top(String(pq.raw)), !fields.isArray,
              let hash = fields.items.first(where: { $0.key == "sha256Hash" })
        else { return nil }
        return JSONSpans.string(hash.raw)
    }

    /// A document has a selection set; "{ x }" is the smallest one.
    private static func looksLikeDocument(_ text: String) -> Bool {
        text.contains("{") && text.contains("}")
    }

    // MARK: - the document

    /// The operation's kind and name, read from the definitions at the top
    /// level of the document: braces, parentheses, strings and comments are
    /// skipped, so a field or argument called `query` is not mistaken for
    /// one. `name` (the request's operationName) picks among several.
    static func fromDocument(_ document: String, name: String) -> GraphQLOperation {
        let defs = definitions(document)
        let chosen = defs.first(where: { !name.isEmpty && $0.name == name })
            ?? defs.first(where: { $0.kind != "fragment" })
        return GraphQLOperation(kind: chosen?.kind ?? "", name: name.isEmpty ? (chosen?.name ?? "") : name,
                                document: document)
    }

    private static func definitions(_ document: String) -> [(kind: String, name: String)] {
        var out: [(kind: String, name: String)] = []
        var words: [String] = []
        var word = ""
        var depth = 0
        var parens = 0
        let chars = Array(document)
        var i = 0
        func endWord() {
            if !word.isEmpty, depth == 0, parens == 0 { words.append(word) }
            word = ""
        }
        while i < chars.count {
            let c = chars[i]
            if c == "#" {
                endWord()
                while i < chars.count && chars[i] != "\n" { i += 1 }
                continue
            }
            if c == "\"" {
                endWord()
                let block = i + 2 < chars.count && chars[i + 1] == "\"" && chars[i + 2] == "\""
                i += block ? 3 : 1
                while i < chars.count {
                    if chars[i] == "\\" { i += 2; continue }
                    if block, i + 2 < chars.count, chars[i] == "\"", chars[i + 1] == "\"", chars[i + 2] == "\"" {
                        i += 3; break
                    }
                    if !block && chars[i] == "\"" { i += 1; break }
                    i += 1
                }
                continue
            }
            if c.isLetter || c.isNumber || c == "_" {
                word.append(c)
                i += 1
                continue
            }
            endWord()
            switch c {
            case "(": parens += 1
            case ")": parens = max(0, parens - 1)
            case "{":
                if depth == 0 && parens == 0 {
                    let kinds = ["query", "mutation", "subscription", "fragment"]
                    if let first = words.first, kinds.contains(first) {
                        out.append((first, words.count > 1 ? words[1] : ""))
                    } else if words.isEmpty {
                        out.append(("query", ""))   // the shorthand "{ ... }"
                    }
                    words = []
                }
                depth += 1
            case "}": depth = max(0, depth - 1)
            default: break
            }
            i += 1
        }
        return out
    }
}

/// The top level of a JSON document as spans of the original text, so a
/// value can be shown exactly as it was sent: parsing to objects and printing
/// back turns 1.0 into 1 and drops a duplicate key (see BodyFormat.prettyJSON).
enum JSONSpans {
    struct Item { let key: String?; let raw: Substring }
    struct Top { let isArray: Bool; let items: [Item] }

    static func top(_ text: String) -> Top? {
        let s = Substring(text)
        var i = skip(s, s.startIndex)
        guard i < s.endIndex, s[i] == "{" || s[i] == "[" else { return nil }
        let isArray = s[i] == "["
        let close: Character = isArray ? "]" : "}"
        i = s.index(after: i)
        var items: [Item] = []
        i = skip(s, i)
        if i < s.endIndex, s[i] == close { return Top(isArray: isArray, items: []) }
        while i < s.endIndex {
            var key: String?
            if !isArray {
                guard s[i] == "\"", let end = valueEnd(s, i) else { return nil }
                key = string(s[i..<end])
                i = skip(s, end)
                guard i < s.endIndex, s[i] == ":" else { return nil }
                i = skip(s, s.index(after: i))
            }
            guard let end = valueEnd(s, i) else { return nil }
            items.append(Item(key: key, raw: s[i..<end]))
            i = skip(s, end)
            guard i < s.endIndex else { return nil }
            if s[i] == "," { i = skip(s, s.index(after: i)); continue }
            if s[i] == close { return Top(isArray: isArray, items: items) }
            return nil
        }
        return nil
    }

    /// A JSON string literal's value, or nil if `raw` is not one.
    static func string(_ raw: Substring) -> String? {
        guard raw.first == "\"",
              let v = try? JSONSerialization.jsonObject(with: Data(raw.utf8), options: .fragmentsAllowed)
        else { return nil }
        return v as? String
    }

    private static func skip(_ s: Substring, _ from: Substring.Index) -> Substring.Index {
        var i = from
        while i < s.endIndex, s[i].isWhitespace { i = s.index(after: i) }
        return i
    }

    /// Where the value starting at `i` ends.
    private static func valueEnd(_ s: Substring, _ start: Substring.Index) -> Substring.Index? {
        guard start < s.endIndex else { return nil }
        var i = start
        if s[i] == "\"" {
            i = s.index(after: i)
            while i < s.endIndex {
                if s[i] == "\\" {
                    i = s.index(after: i)
                    if i < s.endIndex { i = s.index(after: i) }
                    continue
                }
                if s[i] == "\"" { return s.index(after: i) }
                i = s.index(after: i)
            }
            return nil
        }
        if s[i] == "{" || s[i] == "[" {
            var depth = 0
            var inString = false
            while i < s.endIndex {
                let c = s[i]
                if inString {
                    if c == "\\" {
                        i = s.index(after: i)
                        if i < s.endIndex { i = s.index(after: i) }
                        continue
                    }
                    if c == "\"" { inString = false }
                } else if c == "\"" {
                    inString = true
                } else if c == "{" || c == "[" {
                    depth += 1
                } else if c == "}" || c == "]" {
                    depth -= 1
                    if depth == 0 { return s.index(after: i) }
                }
                i = s.index(after: i)
            }
            return nil
        }
        while i < s.endIndex, !",}]".contains(s[i]), !s[i].isWhitespace { i = s.index(after: i) }
        return i == start ? nil : i
    }
}
