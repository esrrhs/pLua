package main

import (
	"bytes"
	"flag"
	"fmt"
	"github.com/goccy/go-graphviz"
	"io/ioutil"
	"os"
)

func main() {

	input := flag.String("i", "", "input file")
	png := flag.String("png", "", "gen png file")

	flag.Parse()

	if len(*input) == 0 {
		flag.Usage()
		os.Exit(1)
	}

	if *png != "" || *input != "" {
		showpng(*input, *png)
	}
}

func showpng(dot string, png string) {
	inputfile, err := os.Open(dot)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	inputdata, err := ioutil.ReadAll(inputfile)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	graph, err := graphviz.ParseBytes(inputdata)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	if png != "" {
		g := graphviz.New()

		var buf bytes.Buffer
		if err := g.Render(graph, graphviz.PNG, &buf); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}

		_, err = g.RenderImage(graph)
		if err != nil {
			fmt.Println(err)
			os.Exit(1)
		}

		if err := g.RenderFilename(graph, graphviz.PNG, png); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
	}
}
